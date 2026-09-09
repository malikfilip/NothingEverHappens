#pragma once

#include <cstdint>

namespace simulator {

    class Event {
    public:
        explicit Event(double time) : time_(time) {}
        virtual ~Event() = default;

        virtual void execute() = 0;

        double time() const { return time_; }
        std::uint64_t sequence() const { return sequence_; }

    private:
        friend class Simulation;

        double time_;
        std::uint64_t sequence_{};
    };

} // namespace simulator
