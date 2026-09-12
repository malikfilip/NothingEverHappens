#include "simulator/Peer.hpp"

#include <stdexcept>
#include <algorithm>
#include <utility>

#include "simulator/Swarm.hpp"
#include "simulator/Message.hpp"

namespace simulator {

    Peer::Peer(std::uint32_t id, double uploadCapacity, double downloadCapacity, PeerProtocolId protocolId)
        : id_(id), protocol_id_(protocolId), upload_capacity_(uploadCapacity), download_capacity_(downloadCapacity)
    {
    }

    std::uint32_t Peer::id() const
    {
        return id_;
    }

    const PeerProtocolId& Peer::protocolId() const
    {
        return protocol_id_;
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
        const auto byteCount = swarm.pieceCount() / 8 + (swarm.pieceCount() % 8 != 0);
        joinSwarm(swarm, std::vector<std::uint8_t>(byteCount, 0));
    }

    void Peer::joinSwarm(const Swarm& swarm, std::vector<std::uint8_t> localBitfield)
    {
        if (hasSwarm(swarm.id())) {
            throw std::invalid_argument("Peer has already joined this SwarmId");
        }
        const auto byteCount = swarm.pieceCount() / 8 + (swarm.pieceCount() % 8 != 0);
        const auto remainder = swarm.pieceCount() % 8;
        if (localBitfield.size() != byteCount
            || (remainder != 0 && (localBitfield.back() & ((1u << (8 - remainder)) - 1u)) != 0)) {
            throw std::invalid_argument("Invalid initial local bitfield");
        }
        swarm_states_.emplace(swarm.id(), PeerSwarmState{std::move(localBitfield), {}});
    }

    bool Peer::hasUsefulPieces(const Swarm& swarm, const PeerSwarmState& state,
                               const PeerConnectionState& remote)
    {
        // Iterate only real pieces, excluding unused trailing bits.
        for (std::uint32_t piece = 0; piece < swarm.pieceCount(); ++piece) {
            const auto mask = 0x80u >> (piece % 8);
            if ((remote.remoteBitfield[piece / 8] & mask) != 0
                && (state.localBitfield[piece / 8] & mask) == 0) {
                return true;
            }
        }
        return false;
    }
    void Peer::validateRequestTo(const Swarm& swarm, const Peer& remote, const RequestPayload& request) const
    {
        if (!hasSwarm(swarm.id()) || !remote.hasSwarm(swarm.id())) {
            throw std::invalid_argument("REQUEST peers must share the swarm");
        }
        const auto& state = swarmState(swarm.id());
        const auto connection = state.connections.find(remote.id());
        if (connection == state.connections.end() || !connection->second.handshakeComplete()
            || !connection->second.weAreInterestedInRemote || connection->second.remoteIsChokingUs) {
            throw std::invalid_argument("REQUEST requires handshake, interest and an unchoked connection");
        }
        const auto size = swarm.pieceSize(request.index);
        if (request.length == 0 || request.begin >= size || request.length > size - request.begin) {
            throw std::invalid_argument("REQUEST block is outside the piece");
        }
        const auto mask = 0x80u >> (request.index % 8);
        if ((state.localBitfield[request.index / 8] & mask) != 0
            || (remote.swarmState(swarm.id()).localBitfield[request.index / 8] & mask) == 0) {
            throw std::invalid_argument("REQUEST requires a missing local piece owned by the remote peer");
        }
    }

    void Peer::receiveMessage(const Swarm& swarm, PeerId sender, const Message& message, const PeerProtocolId* senderProtocolId, const Peer* senderPeer)
    {
        const auto state = swarm_states_.find(swarm.id());
        if (state == swarm_states_.end()) {
            throw std::invalid_argument("Receiving peer has not joined this swarm");
        }
        const auto byteCount = swarm.pieceCount() / 8 + (swarm.pieceCount() % 8 != 0);
        // All validation precedes mutation, including connection creation.
        if (message.type() == MessageType::Handshake) {
            const auto& handshake = std::get<HandshakePayload>(message.payload());
            if (handshake.infoHash != swarm.infoHash()) {
                throw std::invalid_argument("Handshake infoHash does not match the swarm");
            }
            if (!senderProtocolId) {
                throw std::invalid_argument("Handshake requires the actual sender protocol identity");
            }
            if (handshake.peerId != *senderProtocolId) {
                throw std::invalid_argument("Handshake peer_id does not match the actual sender");
            }
        }
        switch (message.type()) {
        case MessageType::Choke:
        case MessageType::Unchoke:
        case MessageType::Interested:
        case MessageType::NotInterested:
        case MessageType::Have:
        case MessageType::Bitfield: {
            const auto connection = state->second.connections.find(sender);
            if (connection == state->second.connections.end() || !connection->second.handshakeComplete()) {
                throw std::invalid_argument("Peer-wire message requires a completed handshake");
            }
            break;
        }
        default:
            break;
        }
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

        if (message.type() == MessageType::Request) {
            if (!senderPeer || senderPeer->id() != sender) {
                throw std::invalid_argument("REQUEST requires actual sender context");
            }
            senderPeer->validateRequestTo(swarm, *this, std::get<RequestPayload>(message.payload()));
            const auto connection = state->second.connections.find(sender);
            if (connection == state->second.connections.end() || !connection->second.handshakeComplete()
                || !connection->second.remoteInterestedInUs || connection->second.weAreChokingRemote) {
                throw std::invalid_argument("REQUEST sender must be interested and unchoked by us");
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
            remote.remoteIsChokingUs = true;
            break;
        case MessageType::Unchoke:
            remote.remoteIsChokingUs = false;
            break;
        case MessageType::Interested:
            remote.remoteInterestedInUs = true;
            remote.weAreChokingRemote = false;
            break;
        case MessageType::NotInterested:
            remote.remoteInterestedInUs = false;
            remote.weAreChokingRemote = true;
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
            remote.handshakeReceived = true;
            break;
        case MessageType::Request: {
            const auto& request = std::get<RequestPayload>(message.payload());
            if (std::find(remote.acceptedRequests.begin(), remote.acceptedRequests.end(), request)
                == remote.acceptedRequests.end()) {
                remote.acceptedRequests.push_back(request);
            }
            break;
        }
        case MessageType::Piece:
        case MessageType::Cancel:
            // Protocol handling is not implemented yet.
            break;
        }
        if (message.type() == MessageType::Bitfield || message.type() == MessageType::Have) {
            remote.weAreInterestedInRemote = hasUsefulPieces(swarm, state->second, remote);
        }
    }

    void Peer::markHandshakeSent(const Swarm& swarm, PeerId receiver)
    {
        const auto state = swarm_states_.find(swarm.id());
        if (state == swarm_states_.end()) {
            throw std::invalid_argument("Sending peer has not joined this swarm");
        }
        const auto byteCount = swarm.pieceCount() / 8 + (swarm.pieceCount() % 8 != 0);
        auto [connection, inserted] = state->second.connections.try_emplace(receiver,
            PeerConnectionState{true, false, std::vector<std::uint8_t>(byteCount, 0)});
        connection->second.handshakeSent = true;
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
