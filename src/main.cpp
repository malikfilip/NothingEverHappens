#include <iostream>
#include <memory>

#include "simulator/Network.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"

int main()
{
    const simulator::Swarm swarm(1, simulator::InfoHash{1}, 9);
    simulator::Peer sender(1, 1000.0, 2000.0, simulator::PeerProtocolId{0xa1});
    simulator::Peer receiver(2, 2000.0, 500.0, simulator::PeerProtocolId{0xb2});
    sender.joinSwarm(swarm);
    receiver.joinSwarm(swarm);

    simulator::Simulation simulation(true);
    simulator::Network network(simulation, {sender, receiver},
        {simulator::Link(1, 2, 800.0, 0.1)}, {swarm});
    simulation.schedule(std::make_unique<simulator::SendMessageEvent>(
        0.0, network, swarm.id(), 1, 2,
        simulator::Message(simulator::MessageType::Handshake,
            simulator::HandshakePayload{swarm.infoHash(), sender.protocolId()})));
    simulation.run();

    const auto& a = network.peer(1).swarmState(swarm.id());
    const auto& b = network.peer(2).swarmState(swarm.id());
    const auto& aToB = a.connections.at(2);
    const auto& bToA = b.connections.at(1);
    const bool complete = aToB.handshakeComplete() && bToA.handshakeComplete();
    const bool exchanged = aToB.bitfieldSent && bToA.bitfieldSent
        && aToB.remoteBitfield == b.localBitfield && bToA.remoteBitfield == a.localBitfield;
    std::cout << std::boolalpha << "Both handshakes complete: " << complete << '\n'
              << "Both automatic BITFIELDs sent: " << (aToB.bitfieldSent && bToA.bitfieldSent) << '\n';
    auto printBitfield = [](const char* label, const std::vector<std::uint8_t>& bytes) {
        std::cout << label;
        for (auto byte : bytes) std::cout << ' ' << static_cast<unsigned>(byte);
        std::cout << '\n';
    };
    printBitfield("Peer 1 stores peer 2 bitfield:", aToB.remoteBitfield);
    printBitfield("Peer 2 stores peer 1 bitfield:", bToA.remoteBitfield);
    std::cout << "Final simulation time: " << simulation.currentTime() << " seconds\n";
    return complete && exchanged ? 0 : 1;
}
