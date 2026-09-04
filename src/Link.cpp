#include "simulator/Link.hpp"

namespace simulator {

    Link::Link(
        std::uint32_t endpoint_a,
        std::uint32_t endpoint_b,
        double bandwidth,
        double latency
    )
        : endpoint_a_(endpoint_a),
          endpoint_b_(endpoint_b),
          bandwidth_(bandwidth),
          latency_(latency)
    {
    }

    std::uint32_t Link::endpointA() const
    {
        return endpoint_a_;
    }

    std::uint32_t Link::endpointB() const
    {
        return endpoint_b_;
    }

    double Link::bandwidth() const
    {
        return bandwidth_;
    }

    double Link::latency() const
    {
        return latency_;
    }

} // namespace simulator
