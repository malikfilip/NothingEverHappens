#pragma once

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "simulator/Peer.hpp"
#include "simulator/Swarm.hpp"

namespace endgame_test {
// Independent interval-union oracle: received bytes plus live reservations must
// cover every missing piece before an exact cross-provider duplicate is legal.
inline bool fullyCovered(const simulator::Swarm& swarm, const simulator::PeerSwarmState& state,
                         std::optional<simulator::RequestPayload> sending = std::nullopt) {
    for (std::uint32_t piece = 0; piece < swarm.pieceCount(); ++piece) {
        if (state.localBitfield[piece / 8] & (0x80u >> (piece % 8))) continue;
        std::vector<simulator::BlockRange> ranges;
        if (const auto it = state.receivedBlocks.find(piece); it != state.receivedBlocks.end()) ranges = it->second;
        for (const auto& [remote, c] : state.connections)
            for (const auto* requests : {&c.scheduledRequests, &c.outgoingRequests})
                for (const auto& block : *requests)
                    if (block.index == piece) ranges.push_back({block.begin, block.begin + block.length});
        if (sending && sending->index == piece) ranges.push_back({sending->begin, sending->begin + sending->length});
        std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.begin < b.begin; });
        std::uint32_t covered = 0;
        for (const auto range : ranges) {
            if (range.begin > covered) return false;
            covered = std::max(covered, range.end);
        }
        if (covered < swarm.pieceSize(piece)) return false;
    }
    return true;
}
inline void checkOverlap(const simulator::Swarm& swarm, const simulator::PeerSwarmState& state,
                         simulator::PeerId remote, const simulator::RequestPayload& block,
                         simulator::PeerId other, const simulator::RequestPayload& earlier,
                         std::optional<simulator::RequestPayload> sending = std::nullopt) {
    if (block.index != earlier.index || block.begin >= earlier.begin + earlier.length
        || earlier.begin >= block.begin + block.length) return;
    if (remote == other || block != earlier || !fullyCovered(swarm, state, sending ? sending : block))
        throw std::runtime_error("Request overlap is not an exact cross-peer endgame duplicate");
}
inline void reservations(const simulator::Swarm& swarm, const simulator::PeerSwarmState& state,
                         std::optional<simulator::RequestPayload> sending = std::nullopt) {
    std::vector<std::pair<simulator::PeerId, simulator::RequestPayload>> seen;
    for (const auto& [remote, c] : state.connections) {
        if (c.scheduledRequests.size() + c.outgoingRequests.size() > 5)
            throw std::runtime_error("Request pipeline exceeds five");
        for (const auto* requests : {&c.scheduledRequests, &c.outgoingRequests})
            for (const auto& block : *requests) {
                for (const auto& [other, earlier] : seen) checkOverlap(swarm, state, remote, block, other, earlier, sending);
                seen.emplace_back(remote, block);
            }
    }
}
}
