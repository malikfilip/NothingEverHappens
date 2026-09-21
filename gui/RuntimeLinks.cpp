#include "RuntimeLinks.hpp"
#include "RuntimeSession.hpp"
#include <map>

RuntimeWireRole runtimeWireRole(const simulator::PeerSwarmState& sender, simulator::PeerId receiver)
{
    const auto connection = sender.connections.find(receiver);
    if (!sender.active || connection == sender.connections.end() || connection->second.weAreChokingRemote)
        return RuntimeWireRole::Choked;
    return sender.choking.optimistic == receiver ? RuntimeWireRole::Optimistic : RuntimeWireRole::Preferred;
}

std::vector<RuntimeWire> inspectRuntimeLinks(const RuntimeSession* runtime,
    quint64 scenarioSwarm, std::optional<quint64> selectedPeer)
{
    if (!runtime || !selectedPeer) return {};
    const auto selected = runtime->peerBindings().find(*selectedPeer);
    if (selected == runtime->peerBindings().end() || selected->second.scenarioSwarmId != scenarioSwarm) return {};
    const auto& binding = selected->second;
    std::map<simulator::PeerId, quint64> scenarioIds;
    for (const auto& [id, peer] : runtime->peerBindings())
        if (peer.scenarioSwarmId == scenarioSwarm) scenarioIds.emplace(peer.peerId, id);
    const auto& network = runtime->network();
    std::vector<RuntimeWire> result;
    const auto append = [&](simulator::PeerId sender, simulator::PeerId receiver) {
        const auto& peer = network.peer(sender);
        result.push_back({scenarioIds.at(sender), scenarioIds.at(receiver),
            peer.hasSwarm(binding.swarmId) ? runtimeWireRole(peer.swarmState(binding.swarmId), receiver)
                                         : RuntimeWireRole::Choked});
    };
    for (const auto& link : network.links()) {
        const auto a = link.endpointA(), b = link.endpointB();
        if (a != binding.peerId && b != binding.peerId) continue;
        if (!scenarioIds.contains(a) || !scenarioIds.contains(b)) continue;
        append(a, b);
        append(b, a);
    }
    return result;
}
