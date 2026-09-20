#pragma once

#include "simulator/BitTorrentSettings.hpp"

// Scenario-wide parameters shared with the engine, never owned by Simulation.
using BitTorrentSettings = simulator::BitTorrentSettings;
struct ScenarioSettings {
    BitTorrentSettings bitTorrent;
};
