#include "simulator/Peer.hpp"

namespace simulator {

    Peer::Peer(std::uint32_t id)
        : id_(id)
    {
    }

    std::uint32_t Peer::id() const
    {
        return id_;
    }

} // namespace simulator