#pragma once
#include "simulator/Peer.hpp"
#include <QtGlobal>
#include <QString>
#include "simulator/Network.hpp"
#include <optional>
#include <vector>
class RuntimeSession;
struct ScenarioSwarm;
// Predicate reads the existing GUI checkboxes; no independent filter state.
using MessageVisibility = std::function<bool(simulator::MessageType)>;
enum class RuntimeWireRole { Choked, Preferred, Optimistic };
struct RuntimeWire {
    quint64 sender;
    quint64 receiver;
    RuntimeWireRole role;
    std::optional<simulator::TransmissionId> active;
    QString activeIcon;
    bool operator==(const RuntimeWire&) const = default;
};
// Presentation classification only; absent connections remain neutral.
RuntimeWireRole runtimeWireRole(const simulator::PeerSwarmState& sender, simulator::PeerId receiver);
// Value snapshots: no engine pointers survive lazy Link creation or session teardown.
std::vector<RuntimeWire> inspectRuntimeLinks(const RuntimeSession* runtime,
    quint64 scenarioSwarm, std::optional<quint64> selectedPeer, const MessageVisibility& visible = {});

struct RuntimeLinkSelection {
    quint64 a, b;
    bool operator==(const RuntimeLinkSelection&) const = default;
};
QString inspectRuntimeLinkHtml(const RuntimeSession& runtime, RuntimeLinkSelection link,
    std::optional<simulator::TransmissionId> message = std::nullopt, const MessageVisibility& visible = {},
    const ScenarioSwarm* scenario = nullptr);
QString inspectTransmissionText(const simulator::ActiveTransmission& active, double now);
QString inspectMessagePayloadText(const simulator::Message& message);
