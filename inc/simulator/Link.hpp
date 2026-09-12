#pragma once

#include <cstdint>
#include <deque>

#include "simulator/Message.hpp"
#include "simulator/Peer.hpp"
#include "simulator/SwarmId.hpp"

namespace simulator {

    struct QueuedTransmission {
        SwarmId swarmId;
        PeerId sender;
        PeerId receiver;
        Message message;
    };

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

        // Bandwidth is bits per second; latency is simulation seconds.
        double bandwidth() const;
        double latency() const;

    private:
        friend class Network;
        struct Direction {
            bool active = false;
            std::deque<QueuedTransmission> pending;
        };
        Direction a_to_b_;
        Direction b_to_a_;

        std::uint32_t endpoint_a_;
        std::uint32_t endpoint_b_;

        double bandwidth_;
        double latency_;
    };

} // namespace simulator
