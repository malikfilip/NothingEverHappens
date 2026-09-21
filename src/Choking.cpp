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
constexpr std::size_t interestedSlots = 4;

double nextDeadline(double previous, double interval)
{
    const double next = previous + interval;
    if (!std::isfinite(next) || next <= previous)
        throw std::invalid_argument("Choking interval cannot advance simulation time");
    return next;
}

void measureRates(PeerSwarmState& state, double deadline, double interval)
{
    auto& policy = state.choking;
    for (auto& [id, connection] : state.connections) {
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
}

// Skip idle wake-ups without moving the original periodic phase. A deadline at
// exactly now is still executed normally, respecting equal-time event ordering.
double skipIdleDeadlines(double deadline, double interval, double now)
{
    if (deadline >= now) return deadline;
    const double next = deadline + std::ceil((now - deadline) / interval) * interval;
    if (!std::isfinite(next) || next < now || next <= deadline)
        throw std::invalid_argument("Choking interval cannot advance simulation time");
    return next;
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

std::optional<PeerId> Network::selectOptimistic(PeerId local, SwarmId swarmId,
    std::vector<PeerId> eligible, std::optional<PeerId> previous)
{
    if (eligible.empty()) return std::nullopt;
    // Canonical enumeration makes replay independent of unordered-map/rate order;
    // IDs do not confer priority. Rotate away from the incumbent when possible.
    std::sort(eligible.begin(), eligible.end());
    if (eligible.size() > 1 && previous) std::erase(eligible, *previous);
    auto& connections = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId).connections;
    std::uint64_t totalWeight = 0;
    for (const auto remote : eligible)
        totalWeight += connections.at(remote).optimisticConsidered ? 1 : 3;

    // BEP 3 gives new connections a threefold preference, but does not define
    // its lifetime. Here it applies to their first eligible lottery, win or lose.
    // Use the same fixed engine/rejection mapping as Tracker for portable replay.
    std::uint64_t ticket = 0;
    if (eligible.size() > 1) {
        const auto threshold = (std::uint64_t{0} - totalWeight) % totalWeight;
        std::uint64_t draw;
        do { draw = optimisticRng_(); } while (draw < threshold);
        ticket = draw % totalWeight;
    }
    std::optional<PeerId> selected;
    for (const auto remote : eligible) {
        auto& connection = connections.at(remote);
        const std::uint64_t weight = connection.optimisticConsidered ? 1 : 3;
        if (!selected) {
            if (ticket < weight) selected = remote;
            else ticket -= weight;
        }
        connection.optimisticConsidered = true;
    }
    return selected;
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

std::vector<PeerId> Network::rankedInterested(PeerId local, SwarmId swarmId, bool interestedOnly) const
{
    const auto& state = peer(local).swarmState(swarmId);
    const bool seed = ownsAll(swarm(swarmId), state);
    std::vector<PeerId> interested;
    for (const auto& [remote, connection] : state.connections)
        if (connection.handshakeComplete() && (!interestedOnly || connection.remoteInterestedInUs)
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
    const auto& state = peer(local).swarmState(swarmId);
    auto& policy = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId).choking;
    const auto ranked = rankedInterested(local, swarmId, false);
    const bool seed = ownsAll(swarm(swarmId), state);
    const auto rate = [&](PeerId id) {
        const auto& connection = state.connections.at(id);
        return seed ? connection.recentUploadRate : connection.recentDownloadRate;
    };
    const auto interested = [&](PeerId id) { return state.connections.at(id).remoteInterestedInUs; };
    const auto eligible = [&](PeerId id) {
        return std::find(ranked.begin(), ranked.end(), id) != ranked.end();
    };
    std::erase_if(policy.preferred, [&](PeerId id) { return !eligible(id); });
    // A regular decision can promote the optimistic peer without a wire transition.
    if (policy.optimistic && (!eligible(*policy.optimistic) || policy.preferred.contains(*policy.optimistic)))
        policy.optimistic.reset();

    const auto selectRegular = [&](std::size_t capacity) {
        std::set<PeerId> selected;
        // Complete the regular decision (also used once at startup).
        for (const bool incumbents : {true, false}) {
            for (const auto remote : ranked) {
                if (selected.size() == capacity) break;
                if (policy.optimistic == remote || !interested(remote)
                    || policy.preferred.contains(remote) != incumbents) continue;
                selected.insert(remote);
            }
        }
        // Uninterested peers above the rate cutoff are pre-unchoked without using
        // an interested slot. With vacancies, any positive measured rate qualifies.
        double cutoff = 0;
        if (selected.size() == capacity) {
            cutoff = std::numeric_limits<double>::infinity();
            for (const auto remote : selected) cutoff = std::min(cutoff, rate(remote));
        }
        for (const auto remote : ranked)
            if (policy.optimistic != remote && !interested(remote) && rate(remote) > cutoff)
                selected.insert(remote);
        policy.preferred = std::move(selected);
    };

    if (rotateOptimistic || !policy.optimistic) {
        // Reserve room for a potentially interested optimistic selection. An
        // uninterested selection gives that fourth interested slot back below.
        selectRegular(interestedSlots - 1);
        std::vector<PeerId> candidates;
        for (const auto remote : ranked)
            if (!policy.preferred.contains(remote)) candidates.push_back(remote);
        policy.optimistic = selectOptimistic(local, swarmId, std::move(candidates), policy.optimistic);
        if (policy.optimistic) policy.optimisticCursor = policy.optimistic;
    }
    selectRegular(interestedSlots - (policy.optimistic && interested(*policy.optimistic) ? 1 : 0));
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

void Network::ensureChokingClock(PeerId local, SwarmId swarmId)
{
    auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
    auto& policy = state.choking;
    if (!policy.managed || policy.eventPending) return;
    const double now = simulation_.currentTime();
    const double interval = bitTorrentSettings_.regularRechokeInterval;
    if (!policy.cycleActive) {
        if (!hasUsefulExchange(local, swarmId)) return;
        const double regular = nextDeadline(now, interval);
        const double optimistic = nextDeadline(now, bitTorrentSettings_.optimisticUnchokeInterval);
        if (policy.cycleGeneration == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Choking cycle generation exhausted");
        ++policy.cycleGeneration;
        policy.cycleActive = true;
        policy.cycleStart = policy.windowStart = now;
        policy.nextRegularDeadline = regular;
        policy.nextOptimisticDeadline = optimistic;
    } else {
        const double regular = skipIdleDeadlines(policy.nextRegularDeadline, interval, now);
        const double optimistic = skipIdleDeadlines(policy.nextOptimisticDeadline,
            bitTorrentSettings_.optimisticUnchokeInterval, now);
        // Age the two-interval history across omitted idle boundaries. After
        // three rolls all buckets/rates are zero; no neighborhood selection runs.
        double boundary = policy.nextRegularDeadline;
        for (unsigned i = 0; i < 3 && boundary < regular; ++i) {
            measureRates(state, boundary, interval);
            boundary = nextDeadline(boundary, interval);
        }
        if (regular != policy.nextRegularDeadline) policy.windowStart = regular - interval;
        policy.nextRegularDeadline = regular;
        policy.nextOptimisticDeadline = optimistic;
    }
    if (hasUsefulExchange(local, swarmId)) scheduleRechoke(local, swarmId);
}

void Network::enforceInterestedBudget(PeerId local, SwarmId swarmId)
{
    auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
    auto& policy = state.choking;
    const auto ranked = rankedInterested(local, swarmId);
    std::size_t count = 0;
    for (const auto remote : ranked)
        count += policy.preferred.contains(remote) || policy.optimistic == remote;
    // Only remove the worst regular incumbents needed to restore the budget.
    // The optimistic assignment is protected; no vacancies are filled here.
    for (auto it = ranked.rbegin(); count > interestedSlots && it != ranked.rend(); ++it)
        if (policy.preferred.erase(*it)) --count;
}

void Network::repairOptimistic(PeerId local, SwarmId swarmId, bool rotate)
{
    auto& policy = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId).choking;
    if (policy.optimistic && !rotate) return;
    auto candidates = rankedInterested(local, swarmId, false);
    std::erase_if(candidates, [&](PeerId remote) { return policy.preferred.contains(remote); });
    policy.optimistic = selectOptimistic(local, swarmId, std::move(candidates), policy.optimistic);
    if (policy.optimistic) policy.optimisticCursor = policy.optimistic;
    enforceInterestedBudget(local, swarmId);
    applyChoking(local, swarmId);
}

void Network::observeInterest(PeerId local, SwarmId swarmId, PeerId remote,
                              bool wasInterested, bool wasChoked)
{
    auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
    auto& policy = state.choking;
    const bool interested = state.connections.at(remote).remoteInterestedInUs;
    if (!policy.managed && interested) {
        // One startup decision per membership, not one bootstrap per arrival.
        policy.managed = true;
        ensureChokingClock(local, swarmId);
        fillChokingSlots(local, swarmId);
        applyChoking(local, swarmId);
    } else if (policy.managed && !wasInterested && interested && !wasChoked) {
        enforceInterestedBudget(local, swarmId);
        applyChoking(local, swarmId);
    }
    // Interest may wake an idle clock, but never restart its phase/history.
    ensureChokingClock(local, swarmId);
}

void Network::recordUsefulPiece(PeerId sender, PeerId receiver, SwarmId swarmId, std::uint64_t bytes)
{
    for (const auto local : {sender, receiver}) {
        ensureChokingClock(local, swarmId); // Accounting/wake-up only; no assignment decision.
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
    if (regular) {
        measureRates(state, deadline, bitTorrentSettings_.regularRechokeInterval);
        const auto interested = rankedInterested(local, swarmId);
        policy.preferred.clear();
        for (const auto remote : interested) {
            if (policy.preferred.size() == interestedSlots - 1) break;
            policy.preferred.insert(remote);
        }
    }
    policy.nextRegularDeadline = nextRegular;
    policy.nextOptimisticDeadline = nextOptimistic;
    if (regular) {
        fillChokingSlots(local, swarmId, optimistic);
        applyChoking(local, swarmId);
    } else if (optimistic) {
        repairOptimistic(local, swarmId, true);
    }
    // Keep the phase/history, but omit redundant idle events so run() can drain.
    if (hasUsefulExchange(local, swarmId)) scheduleRechoke(local, swarmId);
}
}
