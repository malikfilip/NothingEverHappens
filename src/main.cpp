#include <iostream>

#include "simulator/Simulation.hpp"

int main()
{
    simulator::Simulation simulation;

    simulation.schedule(5.0, [] {
        std::cout << "Event A\n";
    });

    simulation.schedule(2.0, [] {
        std::cout << "Event B\n";
    });

    simulation.schedule(2.0, [] {
        std::cout << "Event C\n";
    });

    simulation.schedule(8.0, [] {
        std::cout << "Event D\n";
    });

    simulation.run();

    std::cout << "Final simulation time: "
              << simulation.currentTime()
              << '\n';

    return 0;
}