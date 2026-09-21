#pragma once
#include "simulator/Peer.hpp"
#include <QtGlobal>
#include <optional>
#include <vector>
class RuntimeSession;
enum class RuntimeWireRole { Choked, Preferred, Optimistic };
struct RuntimeWire {
    quint64 sender;
    quint64 receiver;
    RuntimeWireRole role;
    bool operator==(const RuntimeWire&) const = default;
};
// Presentation classification only; absent connections remain neutral.
RuntimeWireRole runtimeWireRole(const simulator::PeerSwarmState& sender, simulator::PeerId receiver);
// Value snapshots: no engine pointers survive lazy Link creation or session teardown.
std::vector<RuntimeWire> inspectRuntimeLinks(const RuntimeSession* runtime,
    quint64 scenarioSwarm, std::optional<quint64> selectedPeer);
