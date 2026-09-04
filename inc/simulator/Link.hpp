#pragma once

#include <cstdint>

namespace simulator {

    class Link {
    public:
        Link(
            std::uint32_t endpoint_a,
            std::uint32_t endpoint_b,
            double bandwidth,
            double latency
        );

        std::uint32_t endpointA() const;
        std::uint32_t endpointB() const;

        double bandwidth() const;
        double latency() const;

    private:
        std::uint32_t endpoint_a_;
        std::uint32_t endpoint_b_;

        double bandwidth_;
        double latency_;
    };

} // namespace simulator