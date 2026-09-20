#include "simulator/Network.hpp"
#include "simulator/RechokeEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace simulator {
namespace {
constexpr std::size_t preferredSlots = 3;

double nextDeadline(double previous, double interval)
{
    const double next = previous + interval;
    if (!std::isfinite(next) || next <= previous)
        throw std::invalid_argument("Choking interval cannot advance simulation time");
    return next;
}

void resetMeasurements(PeerSwarmState& state)
{
    state.choking.previousIntervalMeasured = false;
    for (auto& [id, connection] : state.connections) {
        connection.downloadedInWindow = connection.uploadedInWindow = 0;
        connection.downloadedPreviousInterval = connection.uploadedPreviousInterval = 0;
        connection.recentDownloadRate = connection.recentUploadRate = 0;
    }
}
}
bool Network::ownsAll(const Swarm& currentSwarm, const PeerSwarmState& state)
{
    for (std::uint32_t i = 0; i < currentSwarm.pieceCount(); ++i) {
        if ((state.localBitfield[i / 8] & (0x80u >> (i % 8))) == 0) return false;
    }
    return true;
}

bool Network::hasUsefulExchange(PeerId local, SwarmId swarmId) const
{
    if (swarm(swarmId).totalSize() == 0) return false; // Protocol-only swarms have no transferable payload.
    const auto& state = peer(local).swarmState(swarmId);
    for (const auto& [remote, connection] : state.connections) {
        if (!connection.handshakeComplete() || !peer(remote).isActiveInSwarm(swarmId)) continue;
        const auto& other = peer(remote).swarmState(swarmId);
        for (std::size_t i = 0; i < state.localBitfield.size(); ++i) {
            if (connection.remoteInterestedInUs && (state.localBitfield[i] & ~other.localBitfield[i])) return true;

        }
    }
    return false;
}

std::optional<PeerId> Network::selectOptimistic(std::vector<PeerId> eligible,
                                             std::optional<PeerId> previous)
{
    // Extension point for a future seeded, weighted sampler.
    if (eligible.empty()) return std::nullopt;
    std::sort(eligible.begin(), eligible.end());
    if (previous) {
        const auto next = std::upper_bound(eligible.begin(), eligible.end(), *previous);
        if (next != eligible.end()) return *next;
    }
    return eligible.front();
}

void Network::setChoking(PeerId local, SwarmId swarmId, PeerId remote, bool choke)
{
    auto& connection = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId).connections.at(remote);
    if (connection.weAreChokingRemote == choke) return;
    connection.weAreChokingRemote = choke;
    if (choke) {
        auto& link = links_[linkIndex(local, remote)];
        auto& direction = local == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        auto& requester = peers_[peer_transport_.at(remote).peerIndex].swarm_states_.at(swarmId).connections.at(local);
        // Preserve already-active and propagating PIECEs. Only queued service is
        // discarded, releasing its matching reservations for a future UNCHOKE.
        std::erase_if(direction.pending, [&](const QueuedTransmission& queued) {
            if (queued.swarmId != swarmId || queued.message.type() != MessageType::Piece) return false;
            const auto& piece = std::get<PiecePayload>(queued.message.payload());
            const RequestPayload block{piece.index, piece.begin, piece.length};
            std::erase(connection.acceptedRequests, block);
            std::erase(connection.committedRequests, block);
            std::erase(requester.outgoingRequests, block);
            std::erase(requester.retiredRequests, block);
            return true;
        });
    }
    simulation_.schedule(std::make_unique<SendMessageEvent>(simulation_.currentTime(), *this,
        swarmId, local, remote, Message(choke ? MessageType::Choke : MessageType::Unchoke)));
}

std::vector<PeerId> Network::rankedInterested(PeerId local, SwarmId swarmId) const
{
    const auto& state = peer(local).swarmState(swarmId);
    const bool seed = ownsAll(swarm(swarmId), state);
    std::vector<PeerId> interested;
    for (const auto& [remote, connection] : state.connections)
        if (connection.handshakeComplete() && connection.remoteInterestedInUs
            && peer(remote).isActiveInSwarm(swarmId)) interested.push_back(remote);
    std::sort(interested.begin(), interested.end(), [&](PeerId a, PeerId b) {
        const auto& ca = state.connections.at(a);
        const auto& cb = state.connections.at(b);
        const double ra = seed ? ca.recentUploadRate : ca.recentDownloadRate;
        const double rb = seed ? cb.recentUploadRate : cb.recentDownloadRate;
        return ra != rb ? ra > rb : a < b;
    });
    return interested;
}

void Network::fillChokingSlots(PeerId local, SwarmId swarmId, bool rotateOptimistic)
{
    auto& policy = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId).choking;
    const auto interested = rankedInterested(local, swarmId);
    auto eligible = [&](PeerId id) {
        return std::find(interested.begin(), interested.end(), id) != interested.end();
    };
    std::erase_if(policy.preferred, [&](PeerId id) { return !eligible(id); });
    if (policy.optimistic && (!eligible(*policy.optimistic) || policy.preferred.contains(*policy.optimistic)))
        policy.optimistic.reset();

    // Fill vacancies, never displace valid incumbents between regular decisions.
    for (const auto remote : interested) {
        if (policy.preferred.size() == preferredSlots) break;
        policy.preferred.insert(remote);
    }
    if (policy.optimistic && policy.preferred.contains(*policy.optimistic)) policy.optimistic.reset();
    if (rotateOptimistic || !policy.optimistic) {
        std::vector<PeerId> candidates;
        for (const auto remote : interested)
            if (!policy.preferred.contains(remote)) candidates.push_back(remote);
        policy.optimistic = selectOptimistic(std::move(candidates), policy.optimisticCursor);
        if (policy.optimistic) policy.optimisticCursor = policy.optimistic;
    }
}

void Network::applyChoking(PeerId local, SwarmId swarmId)
{
    const auto& state = peer(local).swarmState(swarmId);
    std::vector<PeerId> established;
    for (const auto& [remote, connection] : state.connections)
        if (connection.handshakeComplete()) established.push_back(remote);
    std::sort(established.begin(), established.end());
    auto selected = [&](PeerId remote) {
        return state.choking.preferred.contains(remote) || state.choking.optimistic == remote;
    };
    // Apply the final assignment once, including when both deadlines coincide.
    for (const auto remote : established) if (!selected(remote)) setChoking(local, swarmId, remote, true);
    for (const auto remote : established) if (selected(remote)) setChoking(local, swarmId, remote, false);
}

void Network::scheduleRechoke(PeerId local, SwarmId swarmId)
{
    auto& policy = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId).choking;
    if (!policy.cycleActive || policy.eventPending) return;
    const double next = std::min(policy.nextRegularDeadline, policy.nextOptimisticDeadline);
    simulation_.schedule(std::make_unique<RechokeEvent>(next, *this, local, swarmId));
    policy.pendingWakeup = next;
    policy.eventPending = true;
}

void Network::updateChoking(PeerId local, SwarmId swarmId)
{
    if (!peer(local).isActiveInSwarm(swarmId)) return;
    auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
    auto& policy = state.choking;
    const bool useful = hasUsefulExchange(local, swarmId);
    if (useful && !policy.cycleActive) {
        const double now = simulation_.currentTime();
        const double regular = nextDeadline(now, bitTorrentSettings_.regularRechokeInterval);
        const double optimistic = nextDeadline(now, bitTorrentSettings_.optimisticUnchokeInterval);
        if (policy.cycleGeneration == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Choking cycle generation exhausted");
        ++policy.cycleGeneration;
        policy.cycleActive = true;
        policy.cycleStart = policy.windowStart = now;
        policy.nextRegularDeadline = regular;
        policy.nextOptimisticDeadline = optimistic;
        resetMeasurements(state);
    } else if (!useful && policy.cycleActive) {
        policy.cycleActive = false;
        policy.eventPending = false;
        policy.pendingWakeup.reset();
        // Old heap events remain harmless: active state and cycle token gate them.
    }
    policy.managed = true;
    fillChokingSlots(local, swarmId);
    applyChoking(local, swarmId);
    scheduleRechoke(local, swarmId);
}

void Network::recordUsefulPiece(PeerId sender, PeerId receiver, SwarmId swarmId, std::uint64_t bytes)
{
    for (const auto local : {sender, receiver}) {
        auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
        auto& connection = state.connections.at(local == sender ? receiver : sender);
        if (local == sender) connection.uploadedInWindow += bytes;
        else connection.downloadedInWindow += bytes;
    }
}

void Network::rechoke(PeerId local, SwarmId swarmId, std::uint64_t cycle, double deadline)
{
    auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
    auto& policy = state.choking;
    // Validate BEFORE touching pending state; duplicate and retired-cycle events are no-ops.
    if (!policy.cycleActive || cycle != policy.cycleGeneration || !policy.eventPending
        || policy.pendingWakeup != deadline || deadline != simulation_.currentTime()) return;
    const bool regular = deadline == policy.nextRegularDeadline;
    const bool optimistic = deadline == policy.nextOptimisticDeadline;
    const double nextRegular = regular
        ? nextDeadline(policy.nextRegularDeadline, bitTorrentSettings_.regularRechokeInterval)
        : policy.nextRegularDeadline;
    const double nextOptimistic = optimistic
        ? nextDeadline(policy.nextOptimisticDeadline, bitTorrentSettings_.optimisticUnchokeInterval)
        : policy.nextOptimisticDeadline;
    policy.eventPending = false;
    policy.pendingWakeup.reset();
    if (!hasUsefulExchange(local, swarmId)) {
        updateChoking(local, swarmId);
        return;
    }
    if (regular) {
        const double interval = bitTorrentSettings_.regularRechokeInterval;
        for (auto& [id, connection] : state.connections) {
            // Divide in two stages to avoid overflow when 2 * interval is nonfinite.
            const double divisor = policy.previousIntervalMeasured ? 2.0 : 1.0;
            connection.recentDownloadRate =
                (static_cast<double>(connection.downloadedPreviousInterval) + connection.downloadedInWindow) / interval / divisor;
            connection.recentUploadRate =
                (static_cast<double>(connection.uploadedPreviousInterval) + connection.uploadedInWindow) / interval / divisor;
            connection.downloadedPreviousInterval = connection.downloadedInWindow;
            connection.uploadedPreviousInterval = connection.uploadedInWindow;
            connection.downloadedInWindow = connection.uploadedInWindow = 0;
        }
        policy.previousIntervalMeasured = true;
        policy.windowStart = deadline;
        const auto interested = rankedInterested(local, swarmId);
        policy.preferred.clear();
        for (const auto remote : interested) {
            if (policy.preferred.size() == preferredSlots) break;
            policy.preferred.insert(remote);
        }
    }
    policy.nextRegularDeadline = nextRegular;
    policy.nextOptimisticDeadline = nextOptimistic;
    fillChokingSlots(local, swarmId, optimistic);
    applyChoking(local, swarmId);
    scheduleRechoke(local, swarmId);
}
}
