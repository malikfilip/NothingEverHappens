#include "RuntimeLinks.hpp"
#include "RuntimeSession.hpp"
#include "MessagePresentation.hpp"
#include <map>
#include <algorithm>
#include <type_traits>
#include "simulator/MessageEventTrace.hpp"

RuntimeWireRole runtimeWireRole(const simulator::PeerSwarmState& sender, simulator::PeerId receiver)
{
    const auto connection = sender.connections.find(receiver);
    if (!sender.active || connection == sender.connections.end() || connection->second.weAreChokingRemote)
        return RuntimeWireRole::Choked;
    return sender.choking.optimistic == receiver ? RuntimeWireRole::Optimistic : RuntimeWireRole::Preferred;
}

std::vector<RuntimeWire> inspectRuntimeLinks(const RuntimeSession* runtime,
    quint64 scenarioSwarm, std::optional<quint64> selectedPeer, const MessageVisibility& visible)
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
    std::map<std::pair<simulator::PeerId, simulator::PeerId>, const simulator::ActiveTransmission*> active;
    for (const auto& [id, tx] : network.activeTransmissions())
        if (!visible || visible(tx.message.type())) active.emplace(std::pair{tx.sender, tx.receiver}, &tx);
    const auto append = [&](simulator::PeerId sender, simulator::PeerId receiver) {
        const auto& peer = network.peer(sender);
        result.push_back({scenarioIds.at(sender), scenarioIds.at(receiver),
            peer.hasSwarm(binding.swarmId) ? runtimeWireRole(peer.swarmState(binding.swarmId), receiver)
                                         : RuntimeWireRole::Choked,
            active.contains({sender, receiver}) ? std::optional{active.at({sender, receiver})->id} : std::nullopt,
            active.contains({sender, receiver}) ? messageIconPath(active.at({sender, receiver})->message.type()) : QString{}});
    };
    for (const auto& link : network.links()) {
        const auto a = link.endpointA(), b = link.endpointB();
        if (a != binding.peerId && b != binding.peerId) continue;
        if (!scenarioIds.contains(a) || !scenarioIds.contains(b)) continue;
        // Physical Links remain allocated across leave/rejoin; inactive membership
        // must not leave a visible connection behind.
        if (!network.peer(a).isActiveInSwarm(binding.swarmId)
            || !network.peer(b).isActiveInSwarm(binding.swarmId)) continue;
        append(a, b);
        append(b, a);
    }
    return result;
}

namespace {
template<class Bytes> QString hexBytes(const Bytes& bytes)
{
    QString text;
    for (auto byte : bytes) text += QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0'));
    return text;
}
}
QString inspectTransmissionText(const simulator::ActiveTransmission& tx, double now)
{
    const double remaining = std::max(0.0, tx.remainingBits - tx.currentRate * (now - tx.lastRateUpdateTime));
    QString text = QStringLiteral("Message: %1\nTransmission ID: %2\nSender (engine): %3\nReceiver (engine): %4\nSwarm (engine): %5\nWire size: %6 bytes\nRate: %7 bit/s\nRemaining: %8 bits\nSimulation time: %9 s\nLast rate update: %10 s\nPredicted completion at current rate: %11 s\n")
        .arg(simulator::messageTypeName(tx.message.type())).arg(tx.id).arg(tx.sender).arg(tx.receiver)
        .arg(tx.swarmId).arg(tx.message.wireSize()).arg(tx.currentRate, 0, 'g', 10)
        .arg(remaining, 0, 'g', 10).arg(now, 0, 'g', 12).arg(tx.lastRateUpdateTime, 0, 'g', 12)
        .arg(tx.lastRateUpdateTime + tx.remainingBits / tx.currentRate, 0, 'g', 12);
    return text + inspectMessagePayloadText(tx.message);
}
QString inspectMessagePayloadText(const simulator::Message& message)
{
    QString text;
    std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, simulator::HandshakePayload>) {
            text += "Info hash (hex): " + hexBytes(payload.infoHash) + "\nPeer ID (hex): " + hexBytes(payload.peerId);
        } else if constexpr (std::is_same_v<T, simulator::BitfieldPayload>) {
            text += "Bitfield (hex, piece 0 = high bit): " + hexBytes(payload.bytes);
            text += "\nRepresented set piece indices: ";
            bool first = true;
            for (std::size_t i = 0; i < payload.bytes.size() * 8; ++i)
                if (payload.bytes[i / 8] & (0x80 >> (i % 8))) {
                    if (!first) text += ", ";
                    text += QString::number(i); first = false;
                }
            if (first) text += "none";
        } else if constexpr (std::is_same_v<T, simulator::HavePayload>) {
            text += QStringLiteral("Piece index: %1").arg(payload.pieceIndex);
        } else if constexpr (std::is_same_v<T, simulator::EmptyPayload>) {
            text += "No payload fields.";
        } else {
            text += QStringLiteral("Index: %1\nBegin: %2\n%3: %4 bytes").arg(payload.index).arg(payload.begin)
                .arg(std::is_same_v<T, simulator::PiecePayload> ? "Simulated payload length" : "Length").arg(payload.length);
        }
    }, message.payload());
    return text;
}
QString inspectRuntimeLinkHtml(const RuntimeSession& runtime, RuntimeLinkSelection selected,
    std::optional<simulator::TransmissionId> message, const MessageVisibility& visible,
    const ScenarioSwarm* scenario)
{
    const auto& bindings = runtime.peerBindings();
    const auto a = bindings.find(selected.a), b = bindings.find(selected.b);
    if (a == bindings.end() || b == bindings.end()) return {};
    const auto& net = runtime.network();
    const auto* link = net.link(a->second.peerId, b->second.peerId);
    if (!link) return {};
    QString html = QStringLiteral("<b>Link</b><br>Endpoints (scenario): %1 &harr; %2<br>Endpoints (engine): %3 &harr; %4<br>Latency: %5 s<br>Capacity per direction: %6 bit/s<br>")
        .arg(selected.a).arg(selected.b).arg(a->second.peerId).arg(b->second.peerId)
        .arg(link->latency(), 0, 'g', 10).arg(link->bandwidth(), 0, 'g', 10);
    const auto name = [scenario](quint64 id) {
        if (scenario) {
            const auto peer = std::find_if(scenario->peers.begin(), scenario->peers.end(),
                [id](const ScenarioPeer& peer) { return peer.id == id; });
            if (peer != scenario->peers.end()) return peer->name.toHtmlEscaped();
        }
        return QString::number(id);
    };
    const auto capacity = [](double bitsPerSecond) {
        double bytes = bitsPerSecond / 8;
        const char* units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s"};
        unsigned unit = 0;
        while (bytes >= 1024 && unit < 3) { bytes /= 1024; ++unit; }
        return QStringLiteral("%1 %2").arg(bytes, 0, 'g', 6).arg(units[unit]);
    };
    for (const auto& pair : {std::pair{a, b}, std::pair{b, a}}) {
        const auto sender = pair.first->second.peerId, receiver = pair.second->second.peerId;
        html += QStringLiteral("<br><b>%1 &rarr; %2</b><br>Sender upload: %3<br>Receiver download: %4<br>Active: ")
            .arg(name(pair.first->first), name(pair.second->first),
                capacity(net.peer(sender).uploadCapacity()), capacity(net.peer(receiver).downloadCapacity()));
        const simulator::ActiveTransmission* active = nullptr;
        for (const auto& [id, tx] : net.activeTransmissions())
            if (tx.sender == sender && tx.receiver == receiver && (!visible || visible(tx.message.type()))) { active = &tx; break; }
        if (active) html += QStringLiteral("<a href=\"%1\"><img src=\"%3\" width=\"20\" height=\"20\"> %2</a>").arg(active->id)
            .arg(simulator::messageTypeName(active->message.type())).arg(messageIconPath(active->message.type()));
        else html += "None";
        html += QStringLiteral("<br>Queued: %1<br>").arg(net.transmissionState(sender, receiver).pendingCount);
        if (active && message == active->id)
            html += "<p>" + inspectTransmissionText(*active, runtime.simulation().currentTime()).toHtmlEscaped().replace("\n", "<br>") + "</p>";
    }
    return html;
}
