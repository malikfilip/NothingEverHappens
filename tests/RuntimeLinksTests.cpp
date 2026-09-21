#include "RuntimeLinks.hpp"
#include "RuntimeSession.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void classification()
{
    simulator::PeerSwarmState state;
    check(runtimeWireRole(state, 2) == RuntimeWireRole::Choked, "Missing connection");
    auto& connection = state.connections[2];
    state.choking.optimistic = 2;
    check(runtimeWireRole(state, 2) == RuntimeWireRole::Choked, "Choke overrides assignment");
    connection.weAreChokingRemote = false;
    check(runtimeWireRole(state, 2) == RuntimeWireRole::Optimistic, "Optimistic assignment");
    state.choking.optimistic.reset();
    check(runtimeWireRole(state, 2) == RuntimeWireRole::Preferred, "Unassigned unchoke fallback");
    state.choking.preferred.insert(2);
    connection.remoteIsChokingUs = true;
    check(runtimeWireRole(state, 2) == RuntimeWireRole::Preferred, "Preferred direction independent of remote choke");
    state.active = false;
    check(runtimeWireRole(state, 2) == RuntimeWireRole::Choked, "Inactive membership");
}
void runtimeSnapshots()
{
    ScenarioSwarm swarm;
    swarm.id = 5000000001ULL;
    swarm.name = "Links";
    swarm.mode = ScenarioSwarm::Mode::Virtual;
    swarm.totalSizeBytes = 1024 * 1024;
    swarm.pieceSizeBytes = 16384;
    swarm.pieceCount = 64;
    for (quint64 i = 0; i < 6; ++i) {
        ScenarioPeer peer;
        peer.id = 9000000001ULL + i;
        peer.name = QString::number(i);
        peer.initiallyJoined = true;
        peer.initialRole = i == 0 ? ScenarioPeer::Role::Seeder : ScenarioPeer::Role::Leecher;
        peer.initialPieceCount = i == 0 ? 64 : 0;
        peer.uploadBytesPerSecond = peer.downloadBytesPerSecond = 1024;
        swarm.peers.push_back(peer);
    }
    std::vector<ScenarioSwarm> scenario{swarm};
    auto runtime = RuntimeSession::create(scenario, {-200, -200, 400, 400});
    const auto selected = swarm.peers[0].id;
    check(inspectRuntimeLinks(runtime.get(), swarm.id, selected).empty(), "Links must be lazy");
    bool sawPreferred = false, sawOptimistic = false, sawAsymmetry = false;
    auto previousOptimistic = std::optional<simulator::PeerId>{};
    bool sawRotation = false;
    std::size_t steps = 0;
    while (runtime->simulation().nextEventTime() && *runtime->simulation().nextEventTime() <= 65) {
        check(++steps < 50000, "Unexpectedly unbounded test");
        runtime->simulation().step();
        const auto& network = runtime->network();
        const auto binding = runtime->peerBindings().at(selected);
        const auto& state = network.peer(binding.peerId).swarmState(binding.swarmId);
        if (state.choking.optimistic) {
            if (previousOptimistic && previousOptimistic != state.choking.optimistic) sawRotation = true;
            previousOptimistic = state.choking.optimistic;
        }
        const auto wires = inspectRuntimeLinks(runtime.get(), swarm.id, selected);
        std::size_t incident = 0;
        for (const auto& link : network.links())
            incident += link.endpointA() == binding.peerId || link.endpointB() == binding.peerId;
        check(wires.size() == 2 * incident, "Exactly two wires per incident physical Link");
        for (std::size_t i = 0; i < wires.size(); i += 2) {
            check(wires[i].sender == wires[i + 1].receiver && wires[i].receiver == wires[i + 1].sender,
                "Full duplex endpoints");
            sawAsymmetry |= wires[i].role != wires[i + 1].role;
        }
        for (const auto& wire : wires) {
            check(wire.sender == selected || wire.receiver == selected, "Non-selected link leaked");
            const auto sender = runtime->peerBindings().at(wire.sender);
            const auto receiver = runtime->peerBindings().at(wire.receiver);
            const auto& actual = network.peer(sender.peerId).swarmState(sender.swarmId);
            const auto connection = actual.connections.find(receiver.peerId);
            const auto expected = connection == actual.connections.end() || connection->second.weAreChokingRemote
                ? RuntimeWireRole::Choked : actual.choking.optimistic == receiver.peerId
                ? RuntimeWireRole::Optimistic : RuntimeWireRole::Preferred;
            check(wire.role == expected, "Live sender state mapping");
            sawPreferred |= wire.role == RuntimeWireRole::Preferred;
            sawOptimistic |= wire.role == RuntimeWireRole::Optimistic;
        }
        const auto time = runtime->simulation().currentTime();
        const auto next = runtime->simulation().nextEventTime();
        const auto linkCount = network.links().size();
        check(inspectRuntimeLinks(runtime.get(), swarm.id, selected) == wires, "Paused snapshot stability");
        check(inspectRuntimeLinks(runtime.get(), swarm.id, std::nullopt).empty(), "Deselection");
        check(inspectRuntimeLinks(runtime.get(), swarm.id + 1, selected).empty(), "Swarm isolation");
        check(inspectRuntimeLinks(runtime.get(), swarm.id, 999).empty(), "Unknown selection");
        for (const auto& wire : inspectRuntimeLinks(runtime.get(), swarm.id, swarm.peers[1].id))
            check(wire.sender == swarm.peers[1].id || wire.receiver == swarm.peers[1].id, "Changed selection");
        check(runtime->simulation().currentTime() == time && runtime->simulation().nextEventTime() == next
            && network.links().size() == linkCount, "Inspection must not mutate runtime");
    }
    check(sawPreferred && sawOptimistic && sawAsymmetry && sawRotation, "Live roles and rotation exercised");
    runtime.reset();
    check(inspectRuntimeLinks(nullptr, swarm.id, selected).empty(), "EDIT/teardown snapshot");
    runtime = RuntimeSession::create(scenario, {-200, -200, 400, 400});
    check(inspectRuntimeLinks(runtime.get(), swarm.id, selected).empty(), "Replacement has no stale links");
}
}
int main()
{
    try { classification(); runtimeSnapshots(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Runtime link tests passed\n";
}
