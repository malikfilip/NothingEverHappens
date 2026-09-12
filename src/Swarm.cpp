#include "simulator/Swarm.hpp"

#include <stdexcept>
#include <limits>

namespace simulator {

    Swarm::Swarm(SwarmId id, InfoHash infoHash, std::uint32_t pieceCount)
        : id_(id), info_hash_(infoHash), piece_count_(pieceCount)
    {
        if (piece_count_ == 0) {
            throw std::invalid_argument("Swarm piece count must be greater than zero");
        }
    }

    Swarm::Swarm(SwarmId id, InfoHash infoHash, std::uint64_t totalSize, std::uint32_t pieceLength)
        : id_(id), info_hash_(infoHash), piece_count_(0),
          total_size_(totalSize), piece_length_(pieceLength)
    {
        if (totalSize == 0 || pieceLength == 0) {
            throw std::invalid_argument("Content size and piece length must be positive");
        }
        const auto count = totalSize / pieceLength + (totalSize % pieceLength != 0);
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Piece count exceeds uint32_t");
        }
        piece_count_ = static_cast<std::uint32_t>(count);
    }

    std::uint64_t Swarm::totalSize() const { return total_size_; }
    std::uint32_t Swarm::pieceLength() const { return piece_length_; }

    std::uint32_t Swarm::pieceSize(std::uint32_t index) const
    {
        if (index >= piece_count_ || piece_length_ == 0) {
            throw std::invalid_argument("Invalid piece index or unavailable piece sizes");
        }
        return index + 1 == piece_count_
            ? static_cast<std::uint32_t>(total_size_ - std::uint64_t(index) * piece_length_)
            : piece_length_;
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
