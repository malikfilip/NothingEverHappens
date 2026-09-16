#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <optional>

#include "simulator/SwarmId.hpp"
#include "simulator/Message.hpp"
#include "simulator/PeerProtocolId.hpp"

namespace simulator {

    using PeerId = std::uint32_t;

    class Swarm;
    class Message;

    struct PeerConnectionState {
        bool remoteIsChokingUs = true;
        bool remoteInterestedInUs = false;
        std::vector<std::uint8_t> remoteBitfield;
        bool handshakeSent = false;
        bool handshakeReceived = false;
        // True once the automatic BITFIELD has been scheduled (it may still be queued).
        bool bitfieldSent = false;
        bool weAreInterestedInRemote = false;
        bool weAreChokingRemote = true;
        // Outgoing requests remain pending until a matching PIECE arrives.
        std::vector<RequestPayload> outgoingRequests;
        std::vector<RequestPayload> acceptedRequests;
        // Reserved by automatic scheduling until the SendMessageEvent executes.
        std::vector<RequestPayload> scheduledRequests;

        // Current and previous globally aligned 10-second buckets.
        std::uint64_t downloadedInWindow = 0;
        std::uint64_t uploadedInWindow = 0;
        std::uint64_t downloadedPreviousInterval = 0;
        std::uint64_t uploadedPreviousInterval = 0;
        double recentDownloadRate = 0;
        double recentUploadRate = 0;

        bool handshakeComplete() const { return handshakeSent && handshakeReceived; }
    };

    struct BlockRange {
        std::uint32_t begin;
        std::uint32_t end; // Exclusive; metadata only.
        bool operator==(const BlockRange&) const = default;
    };

    struct ChokingState {
        bool eventPending = false;
        bool managed = false;
        double windowStart = 0; // Start of the current byte bucket.
        double nextOptimisticRotation = 30;
        std::optional<PeerId> optimistic;
        std::optional<PeerId> optimisticCursor;
    };

    struct PeerSwarmState {

        // Piece 0 is the high bit of byte 0; unused trailing bits are zero.
        std::vector<std::uint8_t> localBitfield;
        std::unordered_map<PeerId, PeerConnectionState> connections;
        std::unordered_map<std::uint32_t, std::vector<BlockRange>> receivedBlocks;
        ChokingState choking;
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
        // Initializes owned pieces; rejects invalid length or unused trailing bits.
        void joinSwarm(const Swarm& swarm, std::vector<std::uint8_t> localBitfield);
        // Throws std::invalid_argument for invalid handshakes, unjoined swarms,
        // ordinary messages before handshake, or invalid HAVE/BITFIELD/REQUEST/PIECE.
        // Network supplies actual sender identity for handshakes and Peer context for REQUEST/PIECE.
        void receiveMessage(const Swarm& swarm, PeerId sender, const Message& message,
                            const PeerProtocolId* senderProtocolId = nullptr, const Peer* senderPeer = nullptr);
        // Called by Network when a validated transfer begins.
        void markHandshakeSent(const Swarm& swarm, PeerId receiver);
        bool hasSwarm(SwarmId swarmId) const;
        // Throws std::out_of_range if this SwarmId has not been joined.
        const PeerSwarmState& swarmState(SwarmId swarmId) const;

    private:
        friend class Network;
        void validatePieceTo(const Swarm& swarm, const Peer& remote, const PiecePayload& piece) const;
        static void validateBlock(const Swarm& swarm, const RequestPayload& block);
        void validateRequestTo(const Swarm& swarm, const Peer& remote, const RequestPayload& request) const;
        static bool hasUsefulPieces(const Swarm& swarm, const PeerSwarmState& state,
                                    const PeerConnectionState& remote);
        std::uint32_t id_;
        PeerProtocolId protocol_id_;
        double upload_capacity_;
        double download_capacity_;
        std::unordered_map<SwarmId, PeerSwarmState> swarm_states_;
    };

} // namespace simulator
