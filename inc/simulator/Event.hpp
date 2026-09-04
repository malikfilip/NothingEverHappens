#pragma once

#include <cstdint>
#include <functional>

namespace simulator {

    struct Event {
        double time;
        std::uint64_t sequence;
        std::function<void()> action;

        bool operator<(const Event& other) const
        {
            if (time != other.time) {
                return time > other.time;
            }

            return sequence > other.sequence;
        }
    };

} // namespace simulator