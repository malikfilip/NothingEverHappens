#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "simulator/Event.hpp"

namespace simulator {

    class Simulation {
    public:
        Simulation();

        void schedule(std::unique_ptr<Event> event);

        void run();

        double currentTime() const;

    private:
        double current_time_;
        std::uint64_t next_sequence_;

        std::vector<std::unique_ptr<Event>> event_queue_;
    };

} // namespace simulator
