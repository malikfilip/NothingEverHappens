#include "simulator/Network.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/TrackerAnnounceEvent.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace simulator {
std::uint64_t Network::lifecycleGeneration(SwarmId swarmId, PeerId local) const {
    const auto found = peer_transport_.find(local);
    if (found == peer_transport_.end()) return 0;
    const auto& states = peers_[found->second.peerIndex].swarm_states_;
    const auto state = states.find(swarmId);
    return state == states.end() ? 0 : state->second.lifecycleGeneration;
}
LifecycleContext Network::lifecycleContext(SwarmId swarmId, PeerId sender, PeerId receiver) const {
    return {lifecycleGeneration(swarmId, sender), lifecycleGeneration(swarmId, receiver)};
}
bool Network::lifecycleCurrent(SwarmId swarmId, PeerId local, std::uint64_t generation) const {
    const auto found = peer_transport_.find(local);
    return found != peer_transport_.end() && peers_[found->second.peerIndex].isActiveInSwarm(swarmId)
        && lifecycleGeneration(swarmId, local) == generation;
}
bool Network::messageStale(SwarmId swarmId, PeerId sender, PeerId receiver, LifecycleContext context) const {
    for (const auto& [local, generation] : {std::pair{sender, context.senderGeneration}, std::pair{receiver, context.receiverGeneration}}) {
        if (lifecycleGeneration(swarmId, local) != generation) return true;
        const auto found = peer_transport_.find(local);
        if (found != peer_transport_.end()) {
            const auto& p = peers_[found->second.peerIndex];
            if (p.hasSwarm(swarmId) && !p.isActiveInSwarm(swarmId)) return true;
        }
    }
    // Never-joined/unknown endpoints still go through existing validation; they
    // are invalid requests, not stale work from a departed membership.
    return false;
}
void Network::joinSwarm(SwarmId swarmId, PeerId local, JoinOptions options) {
    validateTrackerSetup();
    const auto& metadata = swarm(swarmId);
    const auto& current = peer(local);
    if (current.isActiveInSwarm(swarmId)) throw std::invalid_argument("Membership already active");
    if (current.hasSwarm(swarmId) && options.initialBitfield)
        throw std::invalid_argument("Rejoin must preserve downloaded data");
    if (lifecycleGeneration(swarmId, local) == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Membership generation exhausted");
    const auto next = simulation_.currentTime() + tracker_.interval();
    if (!std::isfinite(next) || next <= simulation_.currentTime())
        throw std::invalid_argument("Tracker interval cannot advance simulation time");
    auto& p = peers_[peer_transport_.at(local).peerIndex];
    if (!p.hasSwarm(swarmId)) {
        if (options.initialBitfield) p.joinSwarm(metadata, std::move(*options.initialBitfield));
        else p.joinSwarm(metadata);
    } else {
        auto& state = p.swarm_states_.at(swarmId);
        ++state.lifecycleGeneration;
        state.active = true;
        state.connections.clear();
        state.choking = {};
    }
    auto& discovery = discovery_[{local, swarmId}];
    discovery = {};
    discovery.targetOutgoingConnections = options.targetOutgoingConnections;
    discovery.numwant = options.numwant;
    discovery.periodic = true;
    scheduleTrackerAnnounce(swarmId, local, AnnounceKind::Started, simulation_.currentTime());
}
void Network::scheduleTrackerAnnounce(SwarmId swarmId, PeerId local, AnnounceKind kind, double time) {
    auto& discovery = discovery_.at({local, swarmId});
    if (!peer(local).isActiveInSwarm(swarmId) || !discovery.periodic || discovery.nextAnnounce) return;
    auto event = std::make_unique<TrackerAnnounceEvent>(time, *this, swarmId, local, discovery.numwant, kind, true);
    simulation_.schedule(std::move(event));
    discovery.nextAnnounce = time;
}
void Network::processTrackerAnnounce(SwarmId swarmId, PeerId local, std::size_t numwant, AnnounceKind kind,
                                    std::uint64_t generation, bool periodic, double eventTime) {
    if (!lifecycleCurrent(swarmId, local, generation)) return;
    validateTrackerSetup();
    if (kind == AnnounceKind::Stopped) {
        tracker_.announce(swarm(swarmId).infoHash(), local, 0, kind);
        return;
    }
    if (periodic) {
        auto& discovery = discovery_.at({local, swarmId});
        if (!discovery.periodic || discovery.nextAnnounce != eventTime) return;
        const auto next = simulation_.currentTime() + tracker_.interval();
        if (!std::isfinite(next) || next <= simulation_.currentTime())
            throw std::invalid_argument("Tracker interval cannot advance simulation time");
        discovery.nextAnnounce.reset();
        const auto used = discovery.initiatedNeighbors.size();
        numwant = std::min(numwant, used < discovery.targetOutgoingConnections ? discovery.targetOutgoingConnections - used : 0);
    }
    for (const auto remote : tracker_.announce(swarm(swarmId).infoHash(), local, numwant, kind))
        tryConnectPeer(swarmId, local, remote);
    if (periodic) scheduleTrackerAnnounce(swarmId, local, AnnounceKind::Regular, simulation_.currentTime() + tracker_.interval());
}
void Network::leaveSwarm(SwarmId swarmId, PeerId local) {
    const auto& metadata = swarm(swarmId);
    if (!peer(local).isActiveInSwarm(swarmId)) return;
    if (lifecycleGeneration(swarmId, local) == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Membership generation exhausted");
    tracker_.announce(metadata.infoHash(), local, 0, AnnounceKind::Stopped);
    auto& state = peers_[peer_transport_.at(local).peerIndex].swarm_states_.at(swarmId);
    state.active = false;
    ++state.lifecycleGeneration;
    state.choking = {};
    if (auto found = discovery_.find({local, swarmId}); found != discovery_.end()) {
        found->second.periodic = false;
        found->second.nextAnnounce.reset();
        found->second.pending.clear();
        found->second.initiatedNeighbors.clear();
    }
    // Purge only matching peer/swarm FIFO traffic. Other swarms retain order.
    for (auto& link : links_) {
        if (link.endpointA() != local && link.endpointB() != local) continue;
        for (auto* direction : {&link.a_to_b_, &link.b_to_a_})
            std::erase_if(direction->pending, [=](const QueuedTransmission& queued) { return queued.swarmId == swarmId; });
    }
    std::set<TransmissionId> canceled, affected;
    const auto& transport = peer_transport_.at(local);
    auto candidates = transport.outgoing;
    candidates.insert(transport.incoming.begin(), transport.incoming.end());
    for (const auto id : candidates) {
        const auto& active = active_transmissions_.at(id);
        if (active.swarmId != swarmId) continue;
        canceled.insert(id);
        const auto sharing = affectedTransmissions(active.sender, active.receiver);
        affected.insert(sharing.begin(), sharing.end());
    }
    // Account all affected flows once at their OLD rates, before changing counts.
    accountProgress(affected);
    std::set<std::pair<std::size_t, PeerId>> freed;
    for (const auto id : canceled) {
        const auto active = active_transmissions_.at(id);
        peer_transport_.at(active.sender).outgoing.erase(id);
        peer_transport_.at(active.receiver).incoming.erase(id);
        auto& link = links_[active.linkIndex];
        (active.sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_).active = false;
        freed.emplace(active.linkIndex, active.sender);
        active_transmissions_.erase(id);
        affected.erase(id);
    }
    // Erasing both sides also releases all scheduled/outgoing/accepted requests.
    std::set<PeerId> survivors;
    for (auto& p : peers_) {
        if (p.id() == local || !p.hasSwarm(swarmId)) continue;
        auto& other = p.swarm_states_.at(swarmId);
        if (other.connections.erase(local)) survivors.insert(p.id());
        if (auto found = discovery_.find({p.id(), swarmId}); found != discovery_.end()) {
            found->second.pending.erase(local);
            found->second.initiatedNeighbors.erase(local);
        }
        if (other.choking.optimistic == local) other.choking.optimistic.reset();
        if (other.choking.optimisticCursor == local) other.choking.optimisticCursor.reset();
    }
    state.connections.clear();
    recomputeRates(affected);
    for (const auto& [index, sender] : freed) startTransmission(index, sender);
    for (const auto survivor : survivors) {
        auto& p = peers_[peer_transport_.at(survivor).peerIndex];
        if (!p.isActiveInSwarm(swarmId)) continue;
        std::set<PeerId> remotes;
        for (const auto& [remote, connection] : p.swarmState(swarmId).connections) remotes.insert(remote);
        for (const auto remote : remotes) tryScheduleRequests(p, swarmId, remote);
        if (!remotes.empty() && p.swarmState(swarmId).choking.managed) scheduleRechoke(survivor, swarmId);
    }
}
}
