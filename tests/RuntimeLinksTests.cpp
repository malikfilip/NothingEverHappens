#include "RuntimeLinks.hpp"
#include "RuntimeSession.hpp"
#include "MessagePresentation.hpp"
#include "simulator/MessageEventTrace.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void messageDetails()
{
    using namespace simulator;
    const auto inspect = [](Message message) {
        ActiveTransmission tx{9, 3, 1, 2, std::move(message), 0, 80, 16, 2, 0, {}};
        return inspectTransmissionText(tx, 4);
    };
    check(inspect(Message(MessageType::Request, RequestPayload{7, 128, 64})).contains("Remaining: 48 bits"),
        "Progress must use engine time and last rate update");
    check(inspect(Message(MessageType::Request, RequestPayload{7, 128, 64})).contains("Length: 64 bytes"), "Request fields");
    check(inspect(Message(MessageType::Piece, PiecePayload{7, 128, 64})).contains("Simulated payload length: 64 bytes"), "Piece fields");
    check(inspect(Message(MessageType::Cancel, CancelPayload{7, 128, 64})).contains("Begin: 128"), "Cancel fields");
    check(inspect(Message(MessageType::Have, HavePayload{7})).contains("Piece index: 7"), "Have fields");
    check(inspect(Message(MessageType::Bitfield, BitfieldPayload{{0x81, 0x80}})).contains("0, 7, 8"), "Packed bitfield pieces");
    HandshakePayload handshake;
    handshake.infoHash[0] = 0xab; handshake.peerId[0] = 0xcd;
    const auto text = inspect(Message(MessageType::Handshake, handshake));
    check(text.contains("Info hash (hex): ab") && text.contains("Peer ID (hex): cd")
        && !text.contains("reserved"), "Only modeled handshake fields");
    for (auto type : {MessageType::Choke, MessageType::Unchoke, MessageType::Interested, MessageType::NotInterested})
        check(inspect(Message(type)).contains("No payload fields."), "Control fields");
}
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
        for (const auto& wire : wires) {
            const auto sender = runtime->peerBindings().at(wire.sender).peerId;
            const auto receiver = runtime->peerBindings().at(wire.receiver).peerId;
            check(wire.active.has_value() == network.transmissionState(sender, receiver).active,
                "Icon must match active direction, never queue/propagation");
            const RuntimeLinkSelection selection{wire.sender, wire.receiver};
            const auto html = inspectRuntimeLinkHtml(*runtime, selection, wire.active, {}, &swarm);
            const auto senderName = swarm.peers.at(wire.sender - swarm.peers.front().id).name.toHtmlEscaped();
            const auto receiverName = swarm.peers.at(wire.receiver - swarm.peers.front().id).name.toHtmlEscaped();
            check(html.contains(QStringLiteral("<b>%1 &rarr; %2</b><br>Sender upload: 1 KiB/s<br>Receiver download: 1 KiB/s")
                .arg(senderName, receiverName)), "Direction must show names and endpoint capacities in bytes/s");
            check(html.contains("Endpoints (engine):") && html.contains("Capacity per direction:"),
                "General physical-link attributes must remain");
            check(html.contains(QStringLiteral("Queued: %1").arg(network.transmissionState(sender, receiver).pendingCount)),
                "Inspector queue count");
            if (wire.active) {
                const auto& tx = network.activeTransmissions().at(*wire.active);
                check(tx.sender == sender && tx.receiver == receiver, "Icon direction");
                check(wire.activeIcon == messageIconPath(tx.message.type()) && html.contains(wire.activeIcon),
                    "Canvas and Inspector must use the same message asset");
                const MessageVisibility hiddenType = [type = tx.message.type()](simulator::MessageType candidate) {
                    return candidate != type;
                };
                const auto filtered = inspectRuntimeLinks(runtime.get(), swarm.id, selected, hiddenType);
                check(filtered.size() == wires.size(), "Filtering must retain directional wires");
                for (const auto& candidate : filtered) {
                    if (candidate.active)
                        check(network.activeTransmissions().at(*candidate.active).message.type() != tx.message.type(),
                            "Disabled type retained a packet icon");
                    else check(candidate.activeIcon.isEmpty(), "Hidden packet retained icon path");
                }
                const auto filteredHtml = inspectRuntimeLinkHtml(*runtime, selection, tx.id, hiddenType);
                check(!filteredHtml.contains(QStringLiteral("href=\"%1\"").arg(tx.id))
                    && !filteredHtml.contains("Wire size:")
                    && !filteredHtml.contains(messageIconPath(tx.message.type())),
                    "Disabled message must hide its entry, asset and expanded details");
                check(filteredHtml.contains(QStringLiteral("Queued: %1").arg(network.transmissionState(sender, receiver).pendingCount)),
                    "Filtering must not change queue counts");
                check(html.contains(QStringLiteral("href=\"%1\"").arg(tx.id))
                    && html.contains("Wire size:"), "Clickable actual message details");
            }
            check(!inspectRuntimeLinkHtml(*runtime, selection, 0).contains("Wire size:"),
                "Expired message must not retain details");
        }
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
    const auto physicalLinks = runtime->network().links().size();
    const auto time = runtime->simulation().currentTime();
    runtime->setPeerMembership(swarm.id, selected, false);
    check(inspectRuntimeLinks(runtime.get(), swarm.id, selected).empty(), "Left selected peer retained wires");
    for (const auto& wire : inspectRuntimeLinks(runtime.get(), swarm.id, swarm.peers[1].id))
        check(wire.sender != selected && wire.receiver != selected, "Inactive remote retained wires");
    check(runtime->network().links().size() == physicalLinks && runtime->simulation().currentTime() == time,
        "Wire filtering deleted engine Links or advanced time");
    runtime->setPeerMembership(swarm.id, selected, true);
    check(!inspectRuntimeLinks(runtime.get(), swarm.id, selected).empty(), "Active rejoin not visible");
    runtime.reset();
    check(inspectRuntimeLinks(nullptr, swarm.id, selected).empty(), "EDIT/teardown snapshot");
    runtime = RuntimeSession::create(scenario, {-200, -200, 400, 400});
    check(inspectRuntimeLinks(runtime.get(), swarm.id, selected).empty(), "Replacement has no stale links");
}
}
int main()
{
    try { messageDetails(); classification(); runtimeSnapshots(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Runtime link tests passed\n";
}
