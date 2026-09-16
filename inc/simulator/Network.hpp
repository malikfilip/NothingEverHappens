#pragma once

#include <vector>
#include <optional>
#include <map>
#include <set>

#include "simulator/Link.hpp"
#include "simulator/Message.hpp"
#include "simulator/Peer.hpp"
#include "simulator/Swarm.hpp"
#include "simulator/Tracker.hpp"

namespace simulator {

    class Simulation;
    class TransmissionCompleteEvent;
    class TransmissionStartEvent;

    using TransmissionId = std::uint64_t;

    struct ActiveTransmission {
        TransmissionId id;
        SwarmId swarmId;
        PeerId sender;
        PeerId receiver;
        Message message;
        std::size_t linkIndex;
        double remainingBits; // At lastRateUpdateTime.
        double currentRate; // Bits per simulation second.
        double lastRateUpdateTime;
        std::uint64_t generation;
    };

    class Network {
    public:
        // Simulation must outlive this network, which owns its peers, links, and swarms.
        Network(Simulation& simulation, std::vector<Peer> peers, std::vector<Link> links, std::vector<Swarm> swarms = {}, std::size_t trackerMaximum = 50);

        // Configure before discovery. Default cap is 8 per peer/swarm; zero disables admission.
        // Rejects a cap below existing plus reserved neighbors.
        void setMaxNeighbors(SwarmId swarmId, PeerId peer, std::size_t maximum);
        // One-shot registration/discovery; validates membership and unique swarm info hashes.
        void announceToTracker(SwarmId swarmId, PeerId peer, std::size_t numwant);

        // Enqueues transmission, starting immediately if idle; throws std::invalid_argument for missing endpoints,
        // a missing link, or invalid transfer bandwidth/latency.
        void send(SwarmId swarmId, PeerId sender, PeerId receiver, Message message);

        // Throws std::invalid_argument for an unknown receiver/swarm or rejected message.
        void deliver(SwarmId swarmId, PeerId sender, PeerId receiver, const Message& message);

        // Read-only lookup; throws std::invalid_argument for an unknown ID.
        const Peer& peer(PeerId id) const;
        const Swarm& swarm(SwarmId id) const;

        struct TransmissionState {
            bool active;
            std::size_t pendingCount; // Excludes the active transmission.
        };
        // Read-only snapshot; throws std::invalid_argument for a missing link.
        TransmissionState transmissionState(PeerId sender, PeerId receiver) const;

        // Explicit low-level rate override in bits per simulation second. Automatic sharing
        // recomputes affected rates on active-set changes.
        // Throws std::invalid_argument for an inactive ID, nonpositive/nonfinite rate,
        // or nonfinite predicted completion/arrival time. Rejection leaves state unchanged.
        void setTransmissionRate(TransmissionId id, double newRate);

        // Queued messages have no active record.
        const std::map<TransmissionId, ActiveTransmission>& activeTransmissions() const {
            return active_transmissions_;
        }

    private:
        friend class SendMessageEvent;
        friend class TransmissionCompleteEvent;
        friend class TransmissionStartEvent;
        friend class RechokeEvent;
        void scheduleRechoke(PeerId local, SwarmId swarmId);
        void rechoke(PeerId local, SwarmId swarmId);
        bool hasUsefulExchange(PeerId local, SwarmId swarmId) const;
        static bool ownsAll(const Swarm& swarm, const PeerSwarmState& state);
        static std::optional<PeerId> selectOptimistic(std::vector<PeerId> eligible,
                                                    std::optional<PeerId> previous);
        void setChoking(PeerId local, SwarmId swarmId, PeerId remote, bool choke);
        void recordUsefulPiece(PeerId sender, PeerId receiver, SwarmId swarmId, std::uint64_t bytes);
        void enforceInterestedLimit(PeerId local, SwarmId swarmId);
        // Network owns multiple peers, so the local requester is explicit.
        void tryScheduleRequests(Peer& requester, SwarmId swarmId, PeerId remotePeerId);
        void sendScheduledPiece(SwarmId swarmId, PeerId sender, PeerId receiver, Message message);
        void sendScheduledRequest(SwarmId swarmId, PeerId sender, PeerId receiver, Message message);
        static std::optional<RequestPayload> nextRequestBlock(const Swarm& swarm,
            const PeerSwarmState& state, std::uint32_t piece, std::uint32_t begin = 0);
        static std::optional<std::uint32_t> selectPiece(const Swarm& swarm,
            const PeerSwarmState& state, const PeerConnectionState& connection,
            const PeerSwarmState& remoteState);
        void beginTransmission(QueuedTransmission transmission);
        void sendInitialBitfield(Peer& sender, SwarmId swarmId, PeerId receiver);
        std::size_t linkIndex(PeerId sender, PeerId receiver) const;
        void startTransmission(std::size_t linkIndex, PeerId sender);
        void completeTransmission(TransmissionId id, std::uint64_t generation);

        std::set<TransmissionId> affectedTransmissions(PeerId sender, PeerId receiver) const;
        void accountProgress(const std::set<TransmissionId>& ids);
        double sharedRate(const ActiveTransmission& active) const;
        void recomputeRates(const std::set<TransmissionId>& ids);

        struct PeerTransport {
            std::size_t peerIndex;
            std::set<TransmissionId> outgoing;
            std::set<TransmissionId> incoming;
        };
        std::map<PeerId, PeerTransport> peer_transport_;
        std::map<TransmissionId, ActiveTransmission> active_transmissions_;
        TransmissionId next_transmission_id_ = 1;
        struct DiscoveryState {
            std::size_t maxNeighbors = 8;
            std::set<PeerId> pending;
        };
        void validateTrackerSetup() const;
        std::set<PeerId> neighbors(SwarmId swarmId, PeerId peer) const;
        bool tryConnectPeer(SwarmId swarmId, PeerId local, PeerId remote);
        Tracker tracker_;
        std::map<std::pair<PeerId, SwarmId>, DiscoveryState> discovery_;
        Simulation& simulation_;
        std::vector<Peer> peers_;
        std::vector<Link> links_;
        std::vector<Swarm> swarms_;
    };

} // namespace simulator
