#include "simulator/Network.hpp"
#include "simulator/RechokeEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"
#include <algorithm>
#include <cmath>
#include <memory>

namespace simulator {
namespace {
constexpr double rechokeInterval = 10;
constexpr double optimisticInterval = 30;
constexpr std::size_t interestedSlots = 4;
constexpr double rateMeasurementWindow = 2 * rechokeInterval;

// Advance through unobserved intervals without creating policy timer events.
// A single interval retains its bytes; two or more expire both old buckets.
void advanceRateBuckets(PeerSwarmState& state, double intervalStart)
{
    const double elapsed = intervalStart - state.choking.windowStart;
    if (elapsed < rechokeInterval) return;
    for (auto& [id, connection] : state.connections) {
        connection.downloadedPreviousInterval = elapsed < rateMeasurementWindow ? connection.downloadedInWindow : 0;
        connection.uploadedPreviousInterval = elapsed < rateMeasurementWindow ? connection.uploadedInWindow : 0;
        connection.downloadedInWindow = connection.uploadedInWindow = 0;
    }
    state.choking.windowStart = intervalStart;
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

void Network::scheduleRechoke(PeerId local, SwarmId swarmId)
{
    if (!peer(local).isActiveInSwarm(swarmId)) return;
    auto& policy = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId).choking;
    if (policy.eventPending) return;
    policy.managed = true;
    policy.eventPending = true;
    const double now = simulation_.currentTime();
    // Global logical boundaries keep simultaneous peer decisions reproducible.
    const double next = (std::floor(now / rechokeInterval) + 1) * rechokeInterval;
    advanceRateBuckets(peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId), next - rechokeInterval);
    simulation_.schedule(std::make_unique<RechokeEvent>(next, *this, local, swarmId));
}

void Network::recordUsefulPiece(PeerId sender, PeerId receiver, SwarmId swarmId, std::uint64_t bytes)
{
    const double now = simulation_.currentTime();
    for (const auto local : {sender, receiver}) {
        auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
        // Preserve recent idle-interval bytes, but expire data older than two buckets.
        // A pending boundary event owns the roll at equal timestamps (sequence order).
        if (!state.choking.eventPending) {
            advanceRateBuckets(state, std::floor(now / rechokeInterval) * rechokeInterval);
        }
        auto& connection = state.connections.at(local == sender ? receiver : sender);
        if (local == sender) connection.uploadedInWindow += bytes;
        else connection.downloadedInWindow += bytes;
    }
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
            std::erase(requester.outgoingRequests, block);
            return true;
        });
    }
    simulation_.schedule(std::make_unique<SendMessageEvent>(simulation_.currentTime(), *this,
        swarmId, local, remote, Message(choke ? MessageType::Choke : MessageType::Unchoke)));
}

void Network::enforceInterestedLimit(PeerId local, SwarmId swarmId)
{
    auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
    const bool seed = ownsAll(swarm(swarmId), state);
    std::vector<PeerId> unchoked;
    for (const auto& [id, connection] : state.connections) {
        if (connection.remoteInterestedInUs && !connection.weAreChokingRemote) unchoked.push_back(id);
    }
    std::sort(unchoked.begin(), unchoked.end(), [&](PeerId a, PeerId b) {
        if (a == b) return false;
        if (a == state.choking.optimistic) return true;
        if (b == state.choking.optimistic) return false;
        const auto& ca = state.connections.at(a);
        const auto& cb = state.connections.at(b);
        const double ra = seed ? ca.recentUploadRate : ca.recentDownloadRate;
        const double rb = seed ? cb.recentUploadRate : cb.recentDownloadRate;
        return ra != rb ? ra > rb : a < b;
    });
    for (std::size_t i = interestedSlots; i < unchoked.size(); ++i) setChoking(local, swarmId, unchoked[i], true);
}

void Network::rechoke(PeerId local, SwarmId swarmId)
{
    if (!peer(local).isActiveInSwarm(swarmId)) return;
    auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
    auto& policy = state.choking;
    policy.eventPending = false;
    policy.managed = true;
    const double now = simulation_.currentTime();
    // Keep the just-ended interval current until after measurement. This also
    // accounts for skipped boundaries when the policy timer was suspended.
    advanceRateBuckets(state, now - rechokeInterval);
    const double measurementWindow = std::min(now, rateMeasurementWindow);
    const bool seed = ownsAll(swarm(swarmId), state);
    std::vector<PeerId> interested, established;
    for (auto& [remote, connection] : state.connections) {
        connection.recentDownloadRate = measurementWindow > 0
            ? (static_cast<double>(connection.downloadedPreviousInterval) + connection.downloadedInWindow) / measurementWindow : 0;
        connection.recentUploadRate = measurementWindow > 0
            ? (static_cast<double>(connection.uploadedPreviousInterval) + connection.uploadedInWindow) / measurementWindow : 0;
        if (!connection.handshakeComplete()) continue;
        established.push_back(remote);
        if (connection.remoteInterestedInUs) interested.push_back(remote);
    }
    advanceRateBuckets(state, now);
    std::sort(established.begin(), established.end());
    std::sort(interested.begin(), interested.end(), [&](PeerId a, PeerId b) {
        const auto& ca = state.connections.at(a);
        const auto& cb = state.connections.at(b);
        const double ra = seed ? ca.recentUploadRate : ca.recentDownloadRate;
        const double rb = seed ? cb.recentUploadRate : cb.recentDownloadRate;
        return ra != rb ? ra > rb : a < b;
    });
    const bool useful = hasUsefulExchange(local, swarmId);
    if (now >= policy.nextOptimisticRotation || !policy.optimistic) {
        std::vector<PeerId> eligible;
        for (const auto remote : established) {
            const auto preferredEnd = interested.begin() + std::min(interestedSlots, interested.size());
            if (std::find(interested.begin(), preferredEnd, remote) != preferredEnd) continue;
            if (state.connections.at(remote).remoteInterestedInUs || useful) eligible.push_back(remote);
        }
        policy.optimistic = selectOptimistic(std::move(eligible), policy.optimisticCursor);
        if (policy.optimistic) policy.optimisticCursor = policy.optimistic;
        if (now >= policy.nextOptimisticRotation) {
            policy.nextOptimisticRotation = (std::floor(now / optimisticInterval) + 1) * optimisticInterval;
        }
    }
    if (!useful && interested.empty()) policy.optimistic.reset();
    std::set<PeerId> selected;
    std::size_t regularSlots = interestedSlots;
    if (policy.optimistic) {
        selected.insert(*policy.optimistic);
        if (state.connections.at(*policy.optimistic).remoteInterestedInUs) --regularSlots;
    }
    for (const auto remote : interested) {
        if (remote == policy.optimistic) continue;
        if (regularSlots == 0) break;
        selected.insert(remote);
        --regularSlots;
    }
    // Apply CHOKEs before UNCHOKEs so a transition never exceeds the local slot cap.
    for (const auto remote : established) if (!selected.contains(remote)) setChoking(local, swarmId, remote, true);
    for (const auto remote : established) if (selected.contains(remote)) setChoking(local, swarmId, remote, false);
    if (useful) scheduleRechoke(local, swarmId);
}
}