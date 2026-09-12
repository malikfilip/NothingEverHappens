#pragma once

#include <cstdint>

#include "simulator/InfoHash.hpp"
#include "simulator/SwarmId.hpp"

namespace simulator {

    class Swarm {
    public:
        // Throws std::invalid_argument if pieceCount is zero.
        Swarm(SwarmId id, InfoHash infoHash, std::uint32_t pieceCount);

        // Derives piece count; rejects zero sizes or counts exceeding uint32_t.
        Swarm(SwarmId id, InfoHash infoHash, std::uint64_t totalSize, std::uint32_t pieceLength);
        std::uint64_t totalSize() const;
        std::uint32_t pieceLength() const;
        // Throws std::invalid_argument for an invalid index or a count-only swarm.
        std::uint32_t pieceSize(std::uint32_t index) const;

        SwarmId id() const;
        const InfoHash& infoHash() const;
        std::uint32_t pieceCount() const;

    private:
        SwarmId id_;
        InfoHash info_hash_;
        std::uint32_t piece_count_;
        std::uint64_t total_size_ = 0;
        std::uint32_t piece_length_ = 0;
    };

} // namespace simulator
