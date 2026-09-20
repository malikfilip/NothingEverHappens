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
        // Explicit configuration rejects zero or a block larger than a full piece.
        // The existing constructor retains 16 KiB (clipped even for tiny legacy pieces).
        Swarm(SwarmId id, InfoHash infoHash, std::uint64_t totalSize,
              std::uint32_t pieceLength, std::uint32_t blockSize);
        static constexpr std::uint32_t defaultBlockSize = 16 * 1024;
        std::uint32_t blockSize() const { return block_size_; }
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
        std::uint32_t block_size_ = defaultBlockSize;
    };

} // namespace simulator
