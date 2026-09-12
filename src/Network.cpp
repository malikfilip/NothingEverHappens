#include "simulator/Network.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
#include "simulator/TransmissionStartEvent.hpp"

namespace simulator {

    Network::Network(Simulation& simulation, std::vector<Peer> peers, std::vector<Link> links, std::vector<Swarm> swarms)
        : simulation_(simulation), peers_(std::move(peers)), links_(std::move(links)), swarms_(std::move(swarms))
    {
    }

    const Peer& Network::peer(PeerId id) const
    {
        const auto found = std::find_if(peers_.begin(), peers_.end(),
            [id](const Peer& peer) { return peer.id() == id; });
        if (found == peers_.end()) {
            throw std::invalid_argument("Unknown peer");
        }
        return *found;
    }

    const Swarm& Network::swarm(SwarmId id) const
    {
        const auto found = std::find_if(swarms_.begin(), swarms_.end(),
            [id](const Swarm& swarm) { return swarm.id() == id; });
        if (found == swarms_.end()) {
            throw std::invalid_argument("Unknown swarm");
        }
        return *found;
    }

    void Network::deliver(SwarmId swarmId, PeerId sender, PeerId receiver, const Message& message)
    {
        const auto to = std::find_if(peers_.begin(), peers_.end(),
            [receiver](const Peer& peer) { return peer.id() == receiver; });
        if (to == peers_.end()) {
            throw std::invalid_argument("Unknown receiver");
        }
        const PeerProtocolId* senderProtocolId = nullptr;
        if (message.type() == MessageType::Handshake) {
            senderProtocolId = &peer(sender).protocolId();
        }
        const auto& currentSwarm = swarm(swarmId);
        const bool availability = message.type() == MessageType::Bitfield || message.type() == MessageType::Have;
        const bool interestMessage = message.type() == MessageType::Interested
            || message.type() == MessageType::NotInterested;
        bool wasChoking = true;
        bool wasInterested = false;
        if ((availability || interestMessage) && to->hasSwarm(swarmId)) {
            const auto& connections = to->swarmState(swarmId).connections;
            const auto connection = connections.find(sender);
            if (connection != connections.end()) {
                wasInterested = connection->second.weAreInterestedInRemote;
                wasChoking = connection->second.weAreChokingRemote;
            }
        }
        to->receiveMessage(currentSwarm, sender, message, senderProtocolId,
            message.type() == MessageType::Request ? &peer(sender) : nullptr);
        if (interestMessage) {
            const bool choking = to->swarmState(swarmId).connections.at(sender).weAreChokingRemote;
            if (choking != wasChoking) {
                simulation_.schedule(std::make_unique<SendMessageEvent>(
                    simulation_.currentTime(), *this, swarmId, receiver, sender,
                    Message(choking ? MessageType::Choke : MessageType::Unchoke)));
            }
        }
        if (availability) {
            const bool interested = to->swarmState(swarmId).connections.at(sender).weAreInterestedInRemote;
            if (interested != wasInterested) {
                simulation_.schedule(std::make_unique<SendMessageEvent>(
                    simulation_.currentTime(), *this, swarmId, receiver, sender,
                    Message(interested ? MessageType::Interested : MessageType::NotInterested)));
            }
        }
        if (message.type() == MessageType::Handshake
            && !to->swarmState(swarmId).connections.at(sender).handshakeSent) {
            const auto& link = links_[linkIndex(receiver, sender)];
            const auto& direction = receiver == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
            const bool queued = std::any_of(direction.pending.begin(), direction.pending.end(),
                [swarmId](const QueuedTransmission& transmission) {
                    return transmission.swarmId == swarmId
                        && transmission.message.type() == MessageType::Handshake;
                });
            if (!queued) {
                SendMessageEvent response(simulation_.currentTime(), *this, swarmId, receiver, sender,
                    Message(MessageType::Handshake,
                        HandshakePayload{currentSwarm.infoHash(), to->protocolId()}));
                simulation_.executeNow(response);
            }
        }
        if (message.type() == MessageType::Handshake) {
            sendInitialBitfield(*to, swarmId, sender);
        }
    }

    void Network::sendInitialBitfield(Peer& sender, SwarmId swarmId, PeerId receiver)
    {
        auto& state = sender.swarm_states_.at(swarmId);
        auto& connection = state.connections.at(receiver);
        if (!connection.handshakeComplete() || connection.bitfieldSent) return;

        simulation_.schedule(std::make_unique<SendMessageEvent>(
            simulation_.currentTime(), *this, swarmId, sender.id(), receiver,
            Message(MessageType::Bitfield, BitfieldPayload{state.localBitfield})));
        connection.bitfieldSent = true;
    }

    std::size_t Network::linkIndex(PeerId sender, PeerId receiver) const
    {
        const auto link = std::find_if(links_.begin(), links_.end(),
            [sender, receiver](const Link& candidate) {
                return (candidate.endpointA() == sender && candidate.endpointB() == receiver)
                    || (candidate.endpointA() == receiver && candidate.endpointB() == sender);
            });
        if (link == links_.end()) {
            throw std::invalid_argument("No link connects sender and receiver");
        }
        return static_cast<std::size_t>(link - links_.begin());
    }

    Network::TransmissionState Network::transmissionState(PeerId sender, PeerId receiver) const
    {
        const auto& link = links_[linkIndex(sender, receiver)];
        const auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        return {direction.active, direction.pending.size()};
    }

    void Network::send(SwarmId swarmId, PeerId sender, PeerId receiver, Message message)
    {
        const auto& from = peer(sender);
        const auto& to = peer(receiver);
        const auto index = linkIndex(sender, receiver);
        if (message.type() == MessageType::Handshake) {
            swarm(swarmId);
            if (!from.hasSwarm(swarmId)) {
                throw std::invalid_argument("Sending peer has not joined this swarm");
            }
        }
        if (message.type() == MessageType::Request) {
            const auto& request = std::get<RequestPayload>(message.payload());
            from.validateRequestTo(swarm(swarmId), to, request);
            const auto& pending = from.swarmState(swarmId).connections.at(receiver).outgoingRequests;
            if (std::find(pending.begin(), pending.end(), request) != pending.end()) return;
        }
        const auto request = message.type() == MessageType::Request
            ? std::optional<RequestPayload>(std::get<RequestPayload>(message.payload())) : std::nullopt;
        auto& link = links_[index];
        auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        direction.pending.push_back({swarmId, sender, receiver, std::move(message)});
        if (!direction.active) {
            startTransmission(index, sender);
        }
        if (request) {
            const auto mutableFrom = std::find_if(peers_.begin(), peers_.end(),
                [sender](const Peer& candidate) { return candidate.id() == sender; });
            mutableFrom->swarm_states_.at(swarmId).connections.at(receiver).outgoingRequests.push_back(*request);
        }
    }

    void Network::startTransmission(std::size_t index, PeerId sender)
    {
        auto& link = links_[index];
        auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        if (direction.active || direction.pending.empty()) return;

        // Only the front message is considered. No timing is assigned while queued.
        auto transmission = std::move(direction.pending.front());
        direction.pending.pop_front();
        TransmissionStartEvent event(simulation_.currentTime(), *this, transmission.swarmId,
            transmission.sender, transmission.receiver, std::move(transmission.message));
        simulation_.executeNow(event);
    }

    void Network::beginTransmission(QueuedTransmission transmission)
    {
        const auto sender = transmission.sender;
        const auto index = linkIndex(sender, transmission.receiver);
        auto& link = links_[index];
        auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        if (direction.active) {
            throw std::logic_error("Cannot start a transmission on a busy direction");
        }
        const auto& to = peer(transmission.receiver);
        const auto from = std::find_if(peers_.begin(), peers_.end(),
            [sender](const Peer& candidate) { return candidate.id() == sender; });
        for (double bandwidth : {from->uploadCapacity(), to.downloadCapacity(), link.bandwidth()}) {
            if (!std::isfinite(bandwidth) || bandwidth <= 0.0) {
                throw std::invalid_argument("Transfer bandwidth must be finite and positive");
            }
        }
        if (!std::isfinite(link.latency()) || link.latency() < 0.0) {
            throw std::invalid_argument("Link latency must be finite and nonnegative");
        }
        const double effectiveBandwidth = std::min({
            from->uploadCapacity(), to.downloadCapacity(), link.bandwidth()});
        const double transmissionTime = static_cast<double>(transmission.message.wireSize()) * 8.0
                                      / effectiveBandwidth;
        const double completionTime = simulation_.currentTime() + transmissionTime;
        const double arrivalTime = completionTime + link.latency();
        if (!std::isfinite(completionTime) || !std::isfinite(arrivalTime)) {
            throw std::invalid_argument("Transmission completion and arrival times must be finite");
        }
        const bool isHandshake = transmission.message.type() == MessageType::Handshake;
        if (isHandshake) {
            from->markHandshakeSent(swarm(transmission.swarmId), transmission.receiver);
        }
        direction.active = true;
        simulation_.schedule(std::make_unique<TransmissionCompleteEvent>(
            completionTime, *this, index, sender,
            MessageEventDetails{transmission.swarmId, sender,
                transmission.receiver, transmission.message.type()}));
        simulation_.schedule(std::make_unique<MessageArrivalEvent>(
            arrivalTime, *this, transmission.swarmId, transmission.sender,
            transmission.receiver, std::move(transmission.message)));
        if (isHandshake) {
            sendInitialBitfield(*from, transmission.swarmId, transmission.receiver);
        }
    }

    void Network::completeTransmission(std::size_t index, PeerId sender)
    {
        auto& link = links_[index];
        auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        direction.active = false;
        startTransmission(index, sender);
    }

} // namespace simulator
