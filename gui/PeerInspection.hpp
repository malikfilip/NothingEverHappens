#pragma once
#include "ScenarioSwarm.hpp"
#include <optional>
class RuntimeSession;

// Short-lived, read-only presentation snapshot; never stored back into scenario/engine.
struct PeerInspection {
    bool joined = false;
    bool runtime = false;
    std::optional<std::uint32_t> enginePeerId;
    quint64 pieceCount = 0;
    quint64 ownedPieces = 0;
    std::optional<std::vector<std::uint8_t>> pieces;
    double uploadBytesPerSecond = 0;
    double downloadBytesPerSecond = 0;
    std::size_t connections = 0, established = 0, chokingUs = 0, weChoke = 0;
    std::size_t interestedInUs = 0, weInterested = 0, outstanding = 0, reserved = 0;
    bool complete() const { return pieces.has_value() && pieceCount > 0 && ownedPieces == pieceCount; }
};
PeerInspection inspectPeer(const ScenarioSwarm& swarm, const ScenarioPeer& peer,
                           const RuntimeSession* runtime);
