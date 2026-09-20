#pragma once
#include "ScenarioSwarm.hpp"

// Generate once at configuration time; startup also supports older unprepared scenarios.
// Existing concrete ownership is never replaced or randomized by inspection.
void ensureScenarioPieces(const ScenarioSwarm& swarm, ScenarioPeer& peer, std::uint64_t seed);
