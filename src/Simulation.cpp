#include "simulator/Simulation.hpp"

#include <algorithm>
#include <utility>

namespace simulator {

    namespace {
        bool laterEvent(const std::unique_ptr<Event>& left, const std::unique_ptr<Event>& right)
        {
            if (left->time() != right->time()) {
                return left->time() > right->time();
            }
            return left->sequence() > right->sequence();
        }
    }

    Simulation::Simulation()
        : current_time_(0.0),
          next_sequence_(0)
    {
    }

    void Simulation::schedule(std::unique_ptr<Event> event)
    {
        if (!event || event->time() < currentTime()) return;

        event->sequence_ = next_sequence_++;
        event_queue_.push_back(std::move(event));
        std::push_heap(event_queue_.begin(), event_queue_.end(), laterEvent);
    }

    void Simulation::run()
    {
        while (!event_queue_.empty()) {
            std::pop_heap(event_queue_.begin(), event_queue_.end(), laterEvent);
            auto event = std::move(event_queue_.back());
            event_queue_.pop_back();

            current_time_ = event->time();
            event->execute();
        }
    }

    double Simulation::currentTime() const
    {
        return current_time_;
    }

} // namespace simulator
