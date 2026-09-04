#pragma once

#include <cstdint>

namespace simulator {

    class Peer {
    public:
        explicit Peer(std::uint32_t id);

        std::uint32_t id() const;

    private:
        std::uint32_t id_;
    };

} // namespace simulator