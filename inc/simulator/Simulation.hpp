#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "simulator/Event.hpp"

namespace simulator {

    class Simulation {
    public:
        explicit Simulation(bool tracing = false, std::uint64_t seed = 0);
        std::uint64_t seed() const { return seed_; }
        void setTracing(bool enabled) { tracing_ = enabled; }
        // Called just before execution; the event reference is valid only during the callback.
        void setEventObserver(std::function<void(const Event&)> observer);
        // Dispatch an immediate event through the same tracing/observer path as run().
        // Its timestamp must equal currentTime().
        void executeNow(Event& event);

        void schedule(std::unique_ptr<Event> event);

        void run();

        double currentTime() const;

    private:
        void executeEvent(Event& event);
        std::uint64_t seed_;
        bool tracing_;
        std::function<void(const Event&)> event_observer_;
        double current_time_;
        std::uint64_t next_sequence_;

        std::vector<std::unique_ptr<Event>> event_queue_;
    };

} // namespace simulator
