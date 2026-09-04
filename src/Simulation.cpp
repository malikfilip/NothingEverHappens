#include "simulator/Simulation.hpp"

namespace simulator {

    Simulation::Simulation()
        : current_time_(0.0),
          next_sequence_(0)
    {
    }

    void Simulation::schedule(double time, std::function<void()> action)
    {
        if (time < currentTime() )return;
        Event event{
            time,
            next_sequence_,
            action
        };

        ++next_sequence_;

        event_queue_.push(event);
    }

    void Simulation::run()
    {
        while (!event_queue_.empty()) {
            Event event = event_queue_.top();
            event_queue_.pop();

            current_time_ = event.time;

            event.action();
        }
    }

    double Simulation::currentTime() const
    {
        return current_time_;
    }

} // namespace simulator