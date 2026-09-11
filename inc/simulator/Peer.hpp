#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "simulator/SwarmId.hpp"
#include "simulator/PeerProtocolId.hpp"

namespace simulator {

    using PeerId = std::uint32_t;

    class Swarm;
    class Message;

    struct PeerConnectionState {
        bool remoteChokingUs = true;
        bool remoteInterestedInUs = false;
        std::vector<std::uint8_t> remoteBitfield;
        bool handshakeComplete = false;
    };

    struct PeerSwarmState {
        // Piece 0 is the high bit of byte 0; unused trailing bits are zero.
        std::vector<std::uint8_t> localBitfield;
        std::unordered_map<PeerId, PeerConnectionState> connections;
    };

    class Peer {
    public:
        // Capacities are bits per second; zero means no transfer capacity.
        explicit Peer(std::uint32_t id, double uploadCapacity = 0.0,
                      double downloadCapacity = 0.0, PeerProtocolId protocolId = {});

        std::uint32_t id() const;
        const PeerProtocolId& protocolId() const;
        double uploadCapacity() const;
        double downloadCapacity() const;

        // Throws std::invalid_argument if this SwarmId is already joined.
        void joinSwarm(const Swarm& swarm);
        // Throws std::invalid_argument for invalid handshakes, unjoined swarms,
        // ordinary messages before handshake, or invalid HAVE/BITFIELD.
        // Handshakes require the actual sender identity supplied by Network.
        void receiveMessage(const Swarm& swarm, PeerId sender, const Message& message,
                            const PeerProtocolId* senderProtocolId = nullptr);
        bool hasSwarm(SwarmId swarmId) const;
        // Throws std::out_of_range if this SwarmId has not been joined.
        const PeerSwarmState& swarmState(SwarmId swarmId) const;

    private:
        std::uint32_t id_;
        PeerProtocolId protocol_id_;
        double upload_capacity_;
        double download_capacity_;
        std::unordered_map<SwarmId, PeerSwarmState> swarm_states_;
    };

} // namespace simulator
