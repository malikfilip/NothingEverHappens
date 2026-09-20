#pragma once

#include "ScenarioSwarm.hpp"
#include "simulator/Network.hpp"
#include "simulator/Simulation.hpp"
#include <QRectF>
#include <QStringList>
#include <map>
#include <memory>

struct PlayPreflight {
    QStringList errors;
    QStringList singlePeerSwarms;
    QStringList noLeecherSwarms;
    QStringList skippedSwarms;
    std::vector<quint64> participatingSwarms;
    bool hasWarnings() const {
        return !singlePeerSwarms.isEmpty() || !noLeecherSwarms.isEmpty() || !skippedSwarms.isEmpty();
    }
};

// One run. Network owns all engine peers, swarms, links and its tracker.
// No widgets, event pump, or BitTorrent protocol implementation live here.
class RuntimeSession {
public:
    struct PeerBinding {
        quint64 scenarioSwarmId;
        simulator::SwarmId swarmId;
        simulator::PeerId peerId;
        QPointF normalizedPosition;
        // Also retained for never-joined peers, whose engine membership does not exist yet.
        std::vector<std::uint8_t> initialBitfield;
    };

    static PlayPreflight preflight(const std::vector<ScenarioSwarm>& scenario);
    // Strong transaction: scenario ownership is published only after complete startup.
    static std::unique_ptr<RuntimeSession> create(std::vector<ScenarioSwarm>& scenario,
        const QRectF& visibleCanvas, std::uint64_t seed = 0);

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;
    RuntimeSession(RuntimeSession&&) = delete;
    RuntimeSession& operator=(RuntimeSession&&) = delete;

    simulator::Simulation& simulation() { return simulation_; }
    const simulator::Simulation& simulation() const { return simulation_; }
    const simulator::Network& network() const { return *network_; }
    const auto& swarmIds() const { return swarmIds_; }
    const auto& peerBindings() const { return peerBindings_; }

private:
    RuntimeSession(std::vector<ScenarioSwarm>& scenario, const QRectF& canvas, std::uint64_t seed);
    // Declaration order ensures Simulation outlives Network and its engine references.
    simulator::Simulation simulation_;
    std::map<quint64, simulator::SwarmId> swarmIds_;
    std::map<quint64, PeerBinding> peerBindings_;
    std::unique_ptr<simulator::Network> network_;
};
