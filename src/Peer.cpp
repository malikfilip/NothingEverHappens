#include "simulator/Peer.hpp"

#include <stdexcept>

#include "simulator/Swarm.hpp"

namespace simulator {

    Peer::Peer(std::uint32_t id, double uploadCapacity, double downloadCapacity)
        : id_(id), upload_capacity_(uploadCapacity), download_capacity_(downloadCapacity)
    {
    }

    std::uint32_t Peer::id() const
    {
        return id_;
    }

    double Peer::uploadCapacity() const
    {
        return upload_capacity_;
    }

    double Peer::downloadCapacity() const
    {
        return download_capacity_;
    }

    void Peer::joinSwarm(const Swarm& swarm)
    {
        if (hasSwarm(swarm.id())) {
            throw std::invalid_argument("Peer has already joined this SwarmId");
        }
        const auto byteCount = swarm.pieceCount() / 8 + (swarm.pieceCount() % 8 != 0);
        swarm_states_.emplace(swarm.id(), PeerSwarmState{
            std::vector<std::uint8_t>(byteCount, 0), {}});
    }

    bool Peer::hasSwarm(SwarmId swarmId) const
    {
        return swarm_states_.contains(swarmId);
    }

    const PeerSwarmState& Peer::swarmState(SwarmId swarmId) const
    {
        return swarm_states_.at(swarmId);
    }

} // namespace simulator
