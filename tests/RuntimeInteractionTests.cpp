#include "SimulationView.hpp"
#include "RuntimeSession.hpp"
#include "RuntimePump.hpp"
#include "MessagePresentation.hpp"
#include <QCheckBox>
#include "PeerInspection.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QGraphicsEllipseItem>
#include <QGraphicsItemGroup>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QMenu>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
void check(bool condition, std::source_location line = std::source_location::current())
{
    if (!condition) throw std::runtime_error("Runtime interaction failed at line " + std::to_string(line.line()));
}
class View : public SimulationView {
public:
    using SimulationView::contextMenuEvent;
};
QGraphicsItem* peerNode(View& view, quint64 id)
{
    for (auto* item : view.scene()->items())
        if (dynamic_cast<QGraphicsItemGroup*>(item) && item->toolTip() == QString::number(id)) return item;
    throw std::runtime_error("Missing node");
}
QColor peerColor(QGraphicsItem* node)
{
    for (auto* child : node->childItems())
        if (auto* circle = dynamic_cast<QGraphicsEllipseItem*>(child)) return circle->brush().color();
    throw std::runtime_error("Missing peer circle");
}
std::vector<QPainterPath> wirePaths(View& view)
{
    std::vector<QPainterPath> result;
    for (auto* item : view.scene()->items())
        if (auto* wire = dynamic_cast<QGraphicsPathItem*>(item); wire && !wire->parentItem()) result.push_back(wire->path());
    return result;
}
QMenu* menuFor(View& view, QGraphicsItem* node)
{
    const auto position = view.mapFromScene(node->pos());
    QContextMenuEvent event(QContextMenuEvent::Mouse, position, view.viewport()->mapToGlobal(position));
    view.contextMenuEvent(&event);
    for (auto* menu : view.findChildren<QMenu*>())
        if (menu->isVisible()) return menu;
    throw std::runtime_error("Missing runtime menu");
}
void mouse(View& view, QEvent::Type type, QPoint position, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(position), QPointF(view.viewport()->mapToGlobal(position)),
        button, buttons, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &event);
}
void runtimeInteraction()
{
    ScenarioSwarm swarm;
    swarm.id = 1; swarm.name = "Interaction"; swarm.totalSizeBytes = 65536;
    swarm.pieceSizeBytes = 16384; swarm.pieceCount = 4; swarm.trackerPosition = {0,100};
    for (quint64 id = 1; id <= 3; ++id) {
        ScenarioPeer peer;
        peer.id = id; peer.name = QString::number(id); peer.initiallyJoined = id != 3;
        peer.initialRole = id == 1 ? ScenarioPeer::Role::Seeder : ScenarioPeer::Role::Leecher;
        peer.initialPieceCount = id == 1 ? 4 : 0;
        peer.uploadBytesPerSecond = peer.downloadBytesPerSecond = 1024;
        peer.position = {double(id - 1) * 130 - 130, -40};
        swarm.peers.push_back(peer);
    }
    std::vector<ScenarioSwarm> scenario{swarm};
    View view; view.resize(800,600); view.show();
    QApplication::processEvents();
    auto runtime = RuntimeSession::create(scenario, {-400,-300,800,600});
    view.showSwarm(&scenario[0]);
    QCheckBox messagesEnabled;
    messagesEnabled.setChecked(true);
    const MessageVisibility visible = [&](simulator::MessageType) { return messagesEnabled.isChecked(); };
    const auto refresh = [&] {
        view.refreshPeerColors([&](quint64 id) { return inspectPeer(scenario[0], scenario[0].peers.at(id-1), runtime.get()); });
        const auto wires = inspectRuntimeLinks(runtime.get(), 1, view.selectedPeer(), visible);
        view.refreshRuntimeLinks(wires);
        std::size_t expected = 0, actual = 0;
        for (const auto& wire : wires) expected += wire.active.has_value();
        for (auto* item : view.scene()->items()) {
            if (!item->data(0).isValid()) continue;
            ++actual;
            const auto id = item->data(0).toULongLong();
            check(runtime->network().activeTransmissions().contains(id));
            const auto* icon = dynamic_cast<QGraphicsPixmapItem*>(item);
            check(icon && !icon->pixmap().isNull());
            const auto type = runtime->network().activeTransmissions().at(id).message.type();
            const auto expectedIcon = QPixmap(messageIconPath(type)).scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            check(icon->pixmap().toImage() == expectedIcon.toImage());
        }
        check(actual == expected);
    };
    QObject::connect(&messagesEnabled, &QCheckBox::toggled, &view, [&](bool) { refresh(); });
    for (const auto type : {simulator::MessageType::Handshake, simulator::MessageType::Bitfield,
        simulator::MessageType::Have, simulator::MessageType::Interested, simulator::MessageType::NotInterested,
        simulator::MessageType::Choke, simulator::MessageType::Unchoke, simulator::MessageType::Request,
        simulator::MessageType::Piece, simulator::MessageType::Cancel})
        check(!QPixmap(messageIconPath(type)).isNull());
    RuntimePump pump;
    view.selectionChanged = refresh;
    view.peerMoved = [&](quint64 swarmId, quint64 id, QPointF position) {
        check(swarmId == 1); scenario[0].peers.at(id-1).position = position;
    };
    view.runtimeMembership = [&](quint64 swarmId, quint64 id) { return runtime->peerMembership(swarmId,id); };
    view.runtimeMembershipChanged = [&](quint64 swarmId, quint64 id, bool joined) {
        pump.executeCommand([&] { runtime->setPeerMembership(swarmId,id,joined); });
    };
    pump.stepped = refresh;
    view.setEditingEnabled(false);
    pump.start(runtime->simulation()); pump.pause();
    view.setRuntimePaused(true);
    // Drive only existing engine work until both original peers establish sessions.
    bool connected = false;
    for (unsigned i = 0; i < 1000; ++i) {
        pump.nextEvent();
        const auto& net = runtime->network();
        const auto& a = net.peer(1).swarmState(1).connections;
        const auto& b = net.peer(2).swarmState(1).connections;
        if (a.contains(2) && b.contains(1) && a.at(2).handshakeComplete() && b.at(1).handshakeComplete()) {
            connected = true; break;
        }
    }
    check(connected);
    auto* node = peerNode(view,1);
    auto* inactive = peerNode(view,3);
    check(peerColor(node) == QColor("#49b76b") && peerColor(inactive) == QColor("#dc4c52"));
    const auto position = view.mapFromScene(node->pos());
    mouse(view,QEvent::MouseButtonPress,position,Qt::LeftButton,Qt::LeftButton);
    mouse(view,QEvent::MouseButtonRelease,position,Qt::LeftButton,Qt::NoButton);
    check(view.selectedPeer() == 1 && wirePaths(view).size() == 2);
    const auto filterTime = runtime->simulation().currentTime();
    const auto activeBeforeFilter = runtime->network().activeTransmissions().size();
    messagesEnabled.setChecked(false);
    for (auto* item : view.scene()->items()) check(!item->data(0).isValid());
    check(wirePaths(view).size() == 2 && runtime->simulation().currentTime() == filterTime
        && runtime->network().activeTransmissions().size() == activeBeforeFilter);
    messagesEnabled.setChecked(true);
    const auto paths = wirePaths(view);
    const auto time = runtime->simulation().currentTime();
    const auto next = runtime->simulation().nextEventTime();
    const auto normalized = runtime->peerBindings().at(1).normalizedPosition;
    const auto latency = runtime->network().links().front().latency();
    const auto rate = runtime->network().links().front().bandwidth();
    unsigned events = 0;
    runtime->simulation().setEventObserver([&](const simulator::Event&) { ++events; });
    check(node->flags().testFlag(QGraphicsItem::ItemIsMovable));
    for (auto* item : view.scene()->items())
        if (dynamic_cast<QGraphicsPixmapItem*>(item)) check(!item->flags().testFlag(QGraphicsItem::ItemIsMovable));
    const auto destination = position + QPoint(40,35);
    mouse(view,QEvent::MouseButtonPress,position,Qt::LeftButton,Qt::LeftButton);
    mouse(view,QEvent::MouseMove,destination,Qt::NoButton,Qt::LeftButton);
    mouse(view,QEvent::MouseButtonRelease,destination,Qt::LeftButton,Qt::NoButton);
    check(view.mapFromScene(node->pos()) == destination);
    check(scenario[0].peers[0].position == node->pos() && wirePaths(view) != paths);
    check(events == 0 && runtime->simulation().currentTime() == time
        && runtime->simulation().nextEventTime() == next
        && runtime->peerBindings().at(1).normalizedPosition == normalized
        && runtime->network().links().front().latency() == latency
        && runtime->network().links().front().bandwidth() == rate);
    // Both directional strokes select the same physical link without losing the peer anchor.
    for (const auto& path : wirePaths(view)) {
        const auto a = path.elementAt(0), b = path.elementAt(1);
        const auto midpoint = view.mapFromScene(QPointF((a.x+b.x)/2, (a.y+b.y)/2));
        mouse(view,QEvent::MouseButtonPress,midpoint,Qt::LeftButton,Qt::LeftButton);
        mouse(view,QEvent::MouseButtonRelease,midpoint,Qt::LeftButton,Qt::NoButton);
        check(view.selectedLink() == RuntimeLinkSelection{1,2} && view.selectedPeer() == 1);
    }
    const auto peerPosition = view.mapFromScene(node->pos());
    mouse(view,QEvent::MouseButtonPress,peerPosition,Qt::LeftButton,Qt::LeftButton);
    mouse(view,QEvent::MouseButtonRelease,peerPosition,Qt::LeftButton,Qt::NoButton);
    check(!view.selectedLink() && view.selectedPeer() == 1);
    view.beginPeerPlacement(scenario[0].peers[0]);
    check(!view.isPlacingPeer());
    // Menu state comes from membership, not stale configuration or even paint.
    auto* menu = menuFor(view,node);
    check(menu->actions().size() == 1 && menu->actions()[0]->text() == "Leave swarm");
    menu->actions()[0]->trigger(); menu->close();
    check(!runtime->peerMembership(1,1).value() && peerColor(node) == QColor("#dc4c52")
        && wirePaths(view).empty() && scenario[0].peers.size() == 3
        && scenario[0].peers[0].initiallyJoined && runtime->simulation().currentTime() == time);
    menu = menuFor(view,node);
    check(menu->actions().size() == 1 && menu->actions()[0]->text() == "Join swarm");
    menu->actions()[0]->trigger(); menu->close();
    check(runtime->peerMembership(1,1).value() && peerColor(node) == QColor("#49b76b")
        && runtime->network().peer(1).swarmState(1).connections.empty()
        && runtime->simulation().currentTime() == time);
    // A never-joined peer moves too; future link latency still uses the initial geometry.
    inactive->setPos(inactive->pos() + QPointF(-25,70));
    menu = menuFor(view,inactive);
    check(menu->actions().size() == 1 && menu->actions()[0]->text() == "Join swarm");
    menu->actions()[0]->trigger(); menu->close();
    check(runtime->peerMembership(1,3).value() && peerColor(inactive) == QColor("#f0ce4e"));
    for (unsigned i=0; i<1000 && runtime->network().links().size()<3; ++i) pump.nextEvent();
    check(runtime->network().links().size() == 3);
    for (const auto& link : runtime->network().links()) {
        const auto delta = runtime->peerBindings().at(link.endpointA()).normalizedPosition
            - runtime->peerBindings().at(link.endpointB()).normalizedPosition;
        const double expected = .001 + std::clamp(std::hypot(delta.x(),delta.y())/std::sqrt(2.0),0.0,1.0)*.049;
        check(std::abs(link.latency()-expected)<1e-12);
    }
    view.setRuntimePaused(false);
    check(!node->flags().testFlag(QGraphicsItem::ItemIsMovable));
    const auto frozen = node->pos();
    const auto beforeResume = runtime->simulation().currentTime();
    pump.resume(); pump.pause();
    check(node->pos() == frozen && runtime->simulation().currentTime() == beforeResume);
    // Playing supports membership menus too, but still no create/delete.
    menu = menuFor(view,node);
    check(menu->actions().size() == 1 && menu->actions()[0]->text() == "Leave swarm");
    menu->close();
    view.beginPeerPlacement(scenario[0].peers[0]); check(!view.isPlacingPeer());
    view.runtimeMembership = [](quint64,quint64) { return std::optional<bool>{}; };
    menu = menuFor(view,node);
    check(!menu->actions()[0]->isEnabled()); // Skipped swarm.
    menu->close();
    runtime->snapshotToScenario(scenario);
    check(scenario[0].peers[0].position == frozen);
    pump.stop();
    view.setEditingEnabled(true);
    check(node->flags().testFlag(QGraphicsItem::ItemIsMovable) && wirePaths(view).empty());
}
}
int main(int argc,char** argv)
{
    QApplication application(argc,argv);
    try { runtimeInteraction(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Runtime interaction tests passed\n";
}