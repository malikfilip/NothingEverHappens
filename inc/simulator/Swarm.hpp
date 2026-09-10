#pragma once

#include <cstdint>

#include "simulator/InfoHash.hpp"
#include "simulator/SwarmId.hpp"

namespace simulator {

    class Swarm {
    public:
        // Throws std::invalid_argument if pieceCount is zero.
        Swarm(SwarmId id, InfoHash infoHash, std::uint32_t pieceCount);

        SwarmId id() const;
        const InfoHash& infoHash() const;
        std::uint32_t pieceCount() const;

    private:
        SwarmId id_;
        InfoHash info_hash_;
        std::uint32_t piece_count_;
    };

} // namespace simulator
