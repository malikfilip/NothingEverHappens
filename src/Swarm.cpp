#include "simulator/Swarm.hpp"

#include <stdexcept>

namespace simulator {

    Swarm::Swarm(SwarmId id, InfoHash infoHash, std::uint32_t pieceCount)
        : id_(id), info_hash_(infoHash), piece_count_(pieceCount)
    {
        if (piece_count_ == 0) {
            throw std::invalid_argument("Swarm piece count must be greater than zero");
        }
    }

    SwarmId Swarm::id() const
    {
        return id_;
    }

    const InfoHash& Swarm::infoHash() const
    {
        return info_hash_;
    }

    std::uint32_t Swarm::pieceCount() const
    {
        return piece_count_;
    }

} // namespace simulator
