#include "simulator/Network.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/SendMessageEvent.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

namespace simulator {
    void Network::setLinkConfigProvider(LinkConfigProvider provider) {
        link_config_provider_ = std::move(provider);
    }

    void Network::validateTrackerSetup() const {
        std::map<InfoHash, SwarmId> hashes;
        for (const auto& swarm : swarms_) {
            const auto [entry, inserted] = hashes.emplace(swarm.infoHash(), swarm.id());
            if (!inserted && entry->second != swarm.id())
                throw std::invalid_argument("Tracker requires unambiguous swarm info hashes");
        }
    }

    std::set<PeerId> Network::neighbors(SwarmId swarmId, PeerId local) const {
        std::set<PeerId> result;
        for (const auto& [remote, connection] : peer(local).swarmState(swarmId).connections)
            result.insert(remote);
        const auto found = discovery_.find({local, swarmId});
        if (found != discovery_.end()) result.insert(found->second.pending.begin(), found->second.pending.end());
        return result;
    }

    void Network::setTargetOutgoingConnections(SwarmId swarmId, PeerId local, std::size_t target) {
        validateTrackerSetup();
        swarm(swarmId);
        if (!peer(local).isActiveInSwarm(swarmId)) throw std::invalid_argument("Announcing peer has not joined swarm");
        auto& discovery = discovery_[{local, swarmId}];
        if (discovery.initiatedNeighbors.size() > target)
            throw std::invalid_argument("Target is below current self-initiated relationship count");
        discovery.targetOutgoingConnections = target;
    }

    void Network::announceToTracker(SwarmId swarmId, PeerId local, std::size_t numwant) {
        validateTrackerSetup();
        const auto& currentSwarm = swarm(swarmId);
        if (!peer(local).isActiveInSwarm(swarmId)) throw std::invalid_argument("Announcing peer has not joined swarm");
        for (const auto remote : tracker_.announce(currentSwarm.infoHash(), local, numwant))
            tryConnectPeer(swarmId, local, remote);
    }

    bool Network::tryConnectPeer(SwarmId swarmId, PeerId local, PeerId remote) {
        if (local == remote || !peer(remote).isActiveInSwarm(swarmId)) return false;
        const auto localNeighbors = neighbors(swarmId, local);
        const auto remoteNeighbors = neighbors(swarmId, remote);
        if (localNeighbors.contains(remote) || remoteNeighbors.contains(local)) return false;
        auto& a = discovery_[{local, swarmId}];
        auto& b = discovery_[{remote, swarmId}];
        if (a.initiatedNeighbors.size() >= a.targetOutgoingConnections) return false;
        const auto path = std::find_if(links_.begin(), links_.end(), [=](const Link& link) {
            return (link.endpointA() == local && link.endpointB() == remote)
                || (link.endpointA() == remote && link.endpointB() == local);
        });
        const bool missingPath = path == links_.end();
        if (missingPath && !link_config_provider_) return false;
        // Both handshake directions must be usable before creating a physical path.
        for (const double rate : {peer(local).uploadCapacity(), peer(local).downloadCapacity(),
                                 peer(remote).uploadCapacity(), peer(remote).downloadCapacity()})
            if (!std::isfinite(rate) || rate <= 0) return false;
        const LinkConfig config = missingPath ? link_config_provider_(local, remote)
            : LinkConfig{path->bandwidth(), path->latency()};
        if (!std::isfinite(config.bandwidth) || config.bandwidth <= 0
            || !std::isfinite(config.latency) || config.latency < 0) return false;
        // Append only: active transmissions and scheduled events retain their link indices.
        if (missingPath) links_.emplace_back(local, remote, config.bandwidth, config.latency);
        auto event = std::make_unique<SendMessageEvent>(simulation_.currentTime(), *this, swarmId, local, remote,
            Message(MessageType::Handshake, HandshakePayload{swarm(swarmId).infoHash(), peer(local).protocolId()}));
        try {
            a.initiatedNeighbors.insert(remote);
            a.pending.insert(remote);
            b.pending.insert(local);
            simulation_.schedule(std::move(event));
        } catch (...) {
            a.initiatedNeighbors.erase(remote);
            a.pending.erase(remote);
            b.pending.erase(local);
            throw;
        }
        return true;
    }
}
