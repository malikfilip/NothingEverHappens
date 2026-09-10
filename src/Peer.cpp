#include "simulator/Peer.hpp"

namespace simulator {

    Peer::Peer(std::uint32_t id, double uploadCapacity, double downloadCapacity)
        : id_(id), upload_capacity_(uploadCapacity), download_capacity_(downloadCapacity)
    {
    }

    std::uint32_t Peer::id() const
    {
        return id_;
    }

    double Peer::uploadCapacity() const
    {
        return upload_capacity_;
    }

    double Peer::downloadCapacity() const
    {
        return download_capacity_;
    }

} // namespace simulator
