#include "simulator/Peer.hpp"

#include <stdexcept>

#include "simulator/Swarm.hpp"
#include "simulator/Message.hpp"

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

    void Peer::receiveMessage(const Swarm& swarm, PeerId sender, const Message& message)
    {
        const auto state = swarm_states_.find(swarm.id());
        if (state == swarm_states_.end()) {
            throw std::invalid_argument("Receiving peer has not joined this swarm");
        }
        const auto byteCount = swarm.pieceCount() / 8 + (swarm.pieceCount() % 8 != 0);
        // Validate before changing state, including creating a sender connection.
        if (message.type() == MessageType::Have
            && std::get<HavePayload>(message.payload()).pieceIndex >= swarm.pieceCount()) {
            throw std::invalid_argument("HAVE piece index is outside the swarm");
        }
        if (message.type() == MessageType::Bitfield) {
            const auto& bytes = std::get<BitfieldPayload>(message.payload()).bytes;
            if (bytes.size() != byteCount) {
                throw std::invalid_argument("BITFIELD byte count does not match the swarm");
            }
            const auto remainder = swarm.pieceCount() % 8;
            if (remainder != 0 && (bytes.back() & ((1u << (8 - remainder)) - 1u)) != 0) {
                throw std::invalid_argument("BITFIELD unused trailing bits must be zero");
            }
        }

        auto& connections = state->second.connections;
        auto connection = connections.find(sender);
        if (connection == connections.end()) {
            connection = connections.emplace(sender, PeerConnectionState{
                true, false, std::vector<std::uint8_t>(byteCount, 0)}).first;
        }
        auto& remote = connection->second;
        switch (message.type()) {
        case MessageType::Choke:
            remote.remoteChokingUs = true;
            break;
        case MessageType::Unchoke:
            remote.remoteChokingUs = false;
            break;
        case MessageType::Interested:
            remote.remoteInterestedInUs = true;
            break;
        case MessageType::NotInterested:
            remote.remoteInterestedInUs = false;
            break;
        case MessageType::Bitfield:
            remote.remoteBitfield = std::get<BitfieldPayload>(message.payload()).bytes;
            break;
        case MessageType::Have: {
            const auto index = std::get<HavePayload>(message.payload()).pieceIndex;
            remote.remoteBitfield[index / 8] |= static_cast<std::uint8_t>(0x80u >> (index % 8));
            break;
        }
        case MessageType::Handshake:
        case MessageType::Request:
        case MessageType::Piece:
        case MessageType::Cancel:
            // Protocol handling is not implemented yet.
            break;
        }
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
