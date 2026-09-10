#include <cmath>
#include <iostream>
#include <memory>

#include "simulator/Network.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/SwarmId.hpp"
#include "simulator/Swarm.hpp"

int main()
{
    simulator::Peer peer(1, 1000.0, 2000.0);
    const simulator::Swarm firstSwarm(1, simulator::InfoHash{1}, 8);
    const simulator::Swarm secondSwarm(2, simulator::InfoHash{2}, 9);
    peer.joinSwarm(firstSwarm);
    peer.joinSwarm(secondSwarm);
    std::cout << "Swarm 1 local bitfield: "
              << peer.swarmState(firstSwarm.id()).localBitfield.size() << " bytes\n"
              << "Swarm 2 local bitfield: "
              << peer.swarmState(secondSwarm.id()).localBitfield.size() << " bytes\n";

    simulator::Simulation simulation;
    simulator::Network network(simulation,
        {peer, simulator::Peer(2, 2000.0, 500.0)},
        {simulator::Link(1, 2, 800.0, 0.1)});
    constexpr simulator::SwarmId swarmId = 1;
    const simulator::Message message(simulator::MessageType::Choke);

    simulation.schedule(std::make_unique<simulator::SendMessageEvent>(
        2.0, network, swarmId, 1, 2, message));
    simulation.run();

    // Five bytes at min(1000, 500, 800) bit/s: 0.08 seconds in transmission.
    const double expectedArrival = 2.18;
    std::cout << "Message wire size: " << message.wireSize() << " bytes\n"
              << "Expected arrival time: " << expectedArrival << " seconds\n"
              << "Observed arrival time: " << simulation.currentTime() << " seconds\n";
    if (std::abs(simulation.currentTime() - expectedArrival) > 1e-12) {
        std::cerr << "Arrival time check failed\n";
        return 1;
    }
    return 0;
}
