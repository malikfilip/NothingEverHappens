#pragma once

#include <cstdint>
#include <queue>
#include <functional>

#include "simulator/Event.hpp"

namespace simulator {

    class Simulation {
    public:
        Simulation();

        void schedule(double time, std::function<void()> action);

        void run();

        double currentTime() const;

    private:
        double current_time_;
        std::uint64_t next_sequence_;

        std::priority_queue<Event> event_queue_;
    };

} // namespace simulator