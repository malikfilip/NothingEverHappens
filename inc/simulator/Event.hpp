#pragma once

#include <cstdint>
#include <string>

namespace simulator {

    class Event {
    public:
        explicit Event(double time) : time_(time) {}
        virtual ~Event() = default;

        virtual void execute() = 0;
        virtual std::string traceDescription() const { return "EVENT"; }

        double time() const { return time_; }
        std::uint64_t sequence() const { return sequence_; }

    private:
        friend class Simulation;

        double time_;
        std::uint64_t sequence_{};
    };

} // namespace simulator
