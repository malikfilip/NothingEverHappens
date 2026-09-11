#include "simulator/Network.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/Simulation.hpp"

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
        to->receiveMessage(swarm(swarmId), sender, message, senderProtocolId);
    }

    void Network::send(SwarmId swarmId, PeerId sender, PeerId receiver, Message message)
    {
        const auto from = std::find_if(peers_.begin(), peers_.end(),
            [sender](const Peer& peer) { return peer.id() == sender; });
        const auto to = std::find_if(peers_.begin(), peers_.end(),
            [receiver](const Peer& peer) { return peer.id() == receiver; });
        if (from == peers_.end() || to == peers_.end()) {
            throw std::invalid_argument("Unknown sender or receiver");
        }

        const auto link = std::find_if(links_.begin(), links_.end(),
            [sender, receiver](const Link& candidate) {
                return (candidate.endpointA() == sender && candidate.endpointB() == receiver)
                    || (candidate.endpointA() == receiver && candidate.endpointB() == sender);
            });
        if (link == links_.end()) {
            throw std::invalid_argument("No link connects sender and receiver");
        }

        for (double bandwidth : {from->uploadCapacity(), to->downloadCapacity(), link->bandwidth()}) {
            if (!std::isfinite(bandwidth) || bandwidth <= 0.0) {
                throw std::invalid_argument("Transfer bandwidth must be finite and positive");
            }
        }
        if (!std::isfinite(link->latency()) || link->latency() < 0.0) {
            throw std::invalid_argument("Link latency must be finite and nonnegative");
        }

        const double effectiveBandwidth = std::min({
            from->uploadCapacity(), to->downloadCapacity(), link->bandwidth()});
        const double transmissionTime = static_cast<double>(message.wireSize()) * 8.0
                                      / effectiveBandwidth;
        const double arrivalTime = simulation_.currentTime() + link->latency() + transmissionTime;
        if (!std::isfinite(arrivalTime)) {
            throw std::invalid_argument("Arrival time must be finite");
        }
        simulation_.schedule(std::make_unique<MessageArrivalEvent>(
            arrivalTime, *this, swarmId, sender, receiver, std::move(message)));
    }

} // namespace simulator
