#include <iostream>
#include <memory>

#include "simulator/Network.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"

int main()
{
    const simulator::Swarm swarm(1, simulator::InfoHash{1}, 8 * 16384 + 4096, 16384);
    simulator::Peer sender(1, 1000.0, 2000.0, simulator::PeerProtocolId{0xa1});
    simulator::Peer receiver(2, 2000.0, 500.0, simulator::PeerProtocolId{0xb2});
    sender.joinSwarm(swarm, {0xff, 0x80}); // Peer 1 seeds all nine pieces.
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
    std::cout << "Peer 2 is interested in peer 1: " << bToA.weAreInterestedInRemote << '\n'
              << "Peer 1 received peer 2 interest: " << aToB.remoteInterestedInUs << '\n';
    std::cout << "Peer 1 is choking peer 2: " << aToB.weAreChokingRemote << '\n'
              << "Peer 2 sees peer 1 choking it: " << bToA.remoteIsChokingUs << '\n';
    std::cout << "Peer 1 requests awaiting response: " << aToB.acceptedRequests.size() << '\n';
    const auto& blocks = b.receivedBlocks.at(0);
    std::cout << "Peer 2 received piece 0 range: [" << blocks.front().begin << ", "
              << blocks.front().end << ") bytes\n"
              << "Peer 2 pending requests: " << bToA.outgoingRequests.size() << '\n';
    const bool downloaded = b.localBitfield == a.localBitfield;
    std::cout << "All pieces downloaded: " << downloaded << '\n';
    return downloaded && complete && exchanged && blocks.front().end == swarm.pieceSize(0)
        && bToA.scheduledRequests.empty() && bToA.outgoingRequests.empty()
        && aToB.acceptedRequests.empty() ? 0 : 1;
}
