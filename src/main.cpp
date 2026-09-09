#include <iostream>
#include <memory>

#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"

namespace {
    // Trace execution only in the demo; message event behavior remains a no-op.
    template <typename MessageEvent>
    class TracedEvent : public MessageEvent {
    public:
        using MessageEvent::MessageEvent;

        void execute() override
        {
            MessageEvent::execute();
            std::cout << "Event at time " << this->time()
                      << ", sequence " << this->sequence() << '\n';
        }
    };
}

int main()
{
    simulator::Simulation simulation;
    using Send = TracedEvent<simulator::SendMessageEvent>;
    using Arrival = TracedEvent<simulator::MessageArrivalEvent>;
    const simulator::Message message(simulator::MessageType::Choke);

    simulation.schedule(std::make_unique<Send>(5.0, 1, 2, message));
    simulation.schedule(std::make_unique<Arrival>(2.0, 2, 1, message));
    simulation.schedule(std::make_unique<Send>(2.0, 1, 2, message));
    simulation.schedule(std::make_unique<Arrival>(8.0, 1, 2, message));

    simulation.run();

    std::cout << "Final simulation time: "
              << simulation.currentTime()
              << '\n';

    return 0;
}
