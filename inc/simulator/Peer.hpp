#pragma once

#include <cstdint>

namespace simulator {

    using PeerId = std::uint32_t;

    class Peer {
    public:
        // Capacities are bits per second; zero means no transfer capacity.
        explicit Peer(std::uint32_t id, double uploadCapacity = 0.0,
                      double downloadCapacity = 0.0);

        std::uint32_t id() const;
        double uploadCapacity() const;
        double downloadCapacity() const;

    private:
        std::uint32_t id_;
        double upload_capacity_;
        double download_capacity_;
    };

} // namespace simulator
