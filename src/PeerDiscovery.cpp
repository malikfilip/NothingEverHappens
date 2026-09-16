#include "simulator/Network.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/SendMessageEvent.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace simulator {
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

    void Network::setMaxNeighbors(SwarmId swarmId, PeerId local, std::size_t maximum) {
        validateTrackerSetup();
        swarm(swarmId);
        if (!peer(local).hasSwarm(swarmId)) throw std::invalid_argument("Announcing peer has not joined swarm");
        if (neighbors(swarmId, local).size() > maximum)
            throw std::invalid_argument("Maximum is below current neighborhood size");
        discovery_[{local, swarmId}].maxNeighbors = maximum;
    }

    void Network::announceToTracker(SwarmId swarmId, PeerId local, std::size_t numwant) {
        validateTrackerSetup();
        const auto& currentSwarm = swarm(swarmId);
        if (!peer(local).hasSwarm(swarmId)) throw std::invalid_argument("Announcing peer has not joined swarm");
        for (const auto remote : tracker_.announce(currentSwarm.infoHash(), local, numwant))
            tryConnectPeer(swarmId, local, remote);
    }

    bool Network::tryConnectPeer(SwarmId swarmId, PeerId local, PeerId remote) {
        if (local == remote || !peer(remote).hasSwarm(swarmId)) return false;
        const auto localNeighbors = neighbors(swarmId, local);
        const auto remoteNeighbors = neighbors(swarmId, remote);
        if (localNeighbors.contains(remote) || remoteNeighbors.contains(local)) return false;
        auto& a = discovery_[{local, swarmId}];
        auto& b = discovery_[{remote, swarmId}];
        if (localNeighbors.size() >= a.maxNeighbors || remoteNeighbors.size() >= b.maxNeighbors) return false;
        const auto path = std::find_if(links_.begin(), links_.end(), [=](const Link& link) {
            return (link.endpointA() == local && link.endpointB() == remote)
                || (link.endpointA() == remote && link.endpointB() == local);
        });
        if (path == links_.end()) return false;
        // Both handshake directions must be able to use the existing transport.
        for (const double rate : {path->bandwidth(), peer(local).uploadCapacity(), peer(local).downloadCapacity(),
                                 peer(remote).uploadCapacity(), peer(remote).downloadCapacity()})
            if (!std::isfinite(rate) || rate <= 0) return false;
        if (!std::isfinite(path->latency()) || path->latency() < 0) return false;
        auto event = std::make_unique<SendMessageEvent>(simulation_.currentTime(), *this, swarmId, local, remote,
            Message(MessageType::Handshake, HandshakePayload{swarm(swarmId).infoHash(), peer(local).protocolId()}));
        a.pending.insert(remote);
        try {
            b.pending.insert(local);
            simulation_.schedule(std::move(event));
        } catch (...) {
            a.pending.erase(remote);
            b.pending.erase(local);
            throw;
        }
        return true;
    }
}
