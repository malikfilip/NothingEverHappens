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

    simulator::Simulation simulation;
    simulator::Network network(simulation, {sender, receiver},
        {simulator::Link(1, 2, 800.0, 0.1)}, {swarm});
    simulation.schedule(std::make_unique<simulator::SendMessageEvent>(
        0.0, network, swarm.id(), 1, 2,
        simulator::Message(simulator::MessageType::Handshake,
            simulator::HandshakePayload{swarm.infoHash(), sender.protocolId()})));
    simulation.schedule(std::make_unique<simulator::SendMessageEvent>(
        2.0, network, swarm.id(), 1, 2,
        simulator::Message(simulator::MessageType::Unchoke)));
    simulation.schedule(std::make_unique<simulator::SendMessageEvent>(
        3.0, network, swarm.id(), 1, 2,
        simulator::Message(simulator::MessageType::Have, simulator::HavePayload{8})));
    simulation.run();

    const auto& remote = network.peer(2).swarmState(swarm.id()).connections.at(1);
    std::cout << std::boolalpha
              << "Both peers joined swarm 1: "
              << (network.peer(1).hasSwarm(1) && network.peer(2).hasSwarm(1)) << '\n'
              << "Peer 2 stores remoteChokingUs for peer 1: " << remote.remoteChokingUs << '\n'
              << "HAVE piece 8: byte 1 = " << static_cast<unsigned>(remote.remoteBitfield.at(1)) << '\n'
              << "Final simulation time: " << simulation.currentTime() << " seconds\n";
    return !remote.remoteChokingUs && remote.remoteBitfield == std::vector<std::uint8_t>{0, 0x80}
        ? 0 : 1;
}
