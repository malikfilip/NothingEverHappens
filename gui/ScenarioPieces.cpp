#include "ScenarioPieces.hpp"
#include <limits>
#include <random>
#include <stdexcept>

namespace {
std::vector<std::uint8_t> generatePieces(std::uint32_t count, quint64 owned,
                                        std::uint64_t seed, quint64 swarmId, quint64 peerId)
{
    // Seed a separate stream per scenario peer: order, display names, and tracker
    // RNG consumption cannot change the selected pieces. Rejection avoids modulo bias.
    std::seed_seq sequence{std::uint32_t(seed), std::uint32_t(seed >> 32),
        std::uint32_t(swarmId), std::uint32_t(swarmId >> 32),
        std::uint32_t(peerId), std::uint32_t(peerId >> 32)};
    std::mt19937_64 rng(sequence);
    const bool complement = owned > count / 2;
    std::vector<std::uint8_t> bits((std::uint64_t(count) + 7) / 8, complement ? 0xff : 0);
    if (count % 8) bits.back() &= static_cast<std::uint8_t>(0xffu << (8 - count % 8));
    const auto selectedCount = complement ? count - owned : owned;
    // Floyd sampling using the output bitfield as the selected set; O(min(k,n-k))
    // draws, without allocating an array of every piece index.
    for (std::uint64_t j = count - selectedCount; j < count; ++j) {
        const auto bound = j + 1;
        const auto threshold = (std::uint64_t{0} - bound) % bound;
        std::uint64_t draw;
        do { draw = rng(); } while (draw < threshold);
        auto index = draw % bound;
        const bool owns = (bits[index / 8] & (0x80u >> (index % 8))) != 0;
        if (complement ? !owns : owns) index = j;
        const auto mask = static_cast<std::uint8_t>(0x80u >> (index % 8));
        if (complement) bits[index / 8] &= static_cast<std::uint8_t>(~mask);
        else bits[index / 8] |= mask;
    }
    return bits;
}
}
void ensureScenarioPieces(const ScenarioSwarm& swarm, ScenarioPeer& peer, std::uint64_t seed)
{
    if (swarm.pieceCount == 0 || swarm.pieceCount > std::numeric_limits<std::uint32_t>::max()
        || peer.initialPieceCount > swarm.pieceCount)
        throw std::invalid_argument("Piece count must fit the engine's positive uint32 range.");
    const auto count = peer.initialRole == ScenarioPeer::Role::Seeder ? swarm.pieceCount : peer.initialPieceCount;
    if (!peer.initialBitfield)
        peer.initialBitfield = generatePieces(static_cast<std::uint32_t>(swarm.pieceCount), count, seed, swarm.id, peer.id);
    peer.initialPieceCount = count;
}
