#include "SimulationView.hpp"
#include "ScenarioSwarm.hpp"
#include "PeerInspection.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGraphicsPixmapItem>
#include <QGraphicsItemGroup>
#include <QGraphicsEllipseItem>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QGraphicsPathItem>
#include <utility>
#include <cmath>
#include <map>
#include <QPalette>
#include <QResizeEvent>

namespace {
class RuntimeWireItem : public QGraphicsPathItem {
public:
    RuntimeWireItem(const QPainterPath& path, RuntimeLinkSelection link)
        : QGraphicsPathItem(path), link(link) {}
    QPainterPath shape() const override {
        QPainterPathStroker stroke;
        stroke.setWidth(10);
        return stroke.createStroke(path());
    }
    QRectF boundingRect() const override { return shape().boundingRect(); }
    RuntimeLinkSelection link;
};
QPointF clampedNodePosition(const QGraphicsItem& node, const QPointF& position,
                           const QGraphicsView& view)
{
    // QRectF preserves the full viewport extent (QRect::right() is inclusive).
    // The scene rectangle is bookkeeping, never the source of movement limits.
    const auto visible = view.viewportTransform().inverted().mapRect(QRectF(view.viewport()->rect()));
    const auto rect = node.boundingRect().united(node.childrenBoundingRect());
    const auto clampAxis = [](qreal value, qreal low, qreal high) {
        // Defensive fallback if an unusually large node cannot fit on one axis.
        return low <= high ? qBound(low, value, high) : (low + high) / 2;
    };
    // Nodes are top-level items with no item transforms; their local extents
    // translate directly into scene coordinates at the proposed position.
    return QPointF(clampAxis(position.x(), visible.left() - rect.left(), visible.right() - rect.right()),
        clampAxis(position.y(), visible.top() - rect.top(), visible.bottom() - rect.bottom()));
}
}

class PeerNode : public QGraphicsItemGroup {
public:
    PeerNode(const ScenarioPeer& peer, const QGraphicsView& view)
        : id(peer.id), view_(view), initiallyJoined_(peer.initiallyJoined)
    {
        circle_ = new QGraphicsEllipseItem(QRectF(-16, -16, 32, 32));
        applyColors();
        addToGroup(circle_);

        const QFont font = QApplication::font();
        const QFontMetricsF metrics(font);
        auto* label = new QGraphicsSimpleTextItem(metrics.elidedText(peer.name, Qt::ElideMiddle, 140));
        label->setFont(font);
        label->setBrush(QApplication::palette().brush(QPalette::Text));
        const auto labelBounds = label->boundingRect();
        label->setPos(-labelBounds.center().x(), 21 - labelBounds.top());
        addToGroup(label);

        // Native items account for pen strokes and text glyphs in their bounds.
        // The group routes circle/label drags to one node and paints no outline.
        setFlags(ItemIsMovable | ItemSendsGeometryChanges);
        setAcceptedMouseButtons(Qt::LeftButton);
        setCursor(Qt::OpenHandCursor);
        setToolTip(peer.name);
    }
    bool initiallyJoined() const { return initiallyJoined_; }
    void setInitiallyJoined(bool joined)
    {
        initiallyJoined_ = joined;
        setVisualState(joined, complete_);
    }
    void setVisualState(bool joined, bool complete) {
        if (joined_ == joined && complete_ == complete) return;
        joined_ = joined;
        complete_ = complete;
        applyColors(); // QGraphicsEllipseItem brush/pen changes schedule its repaint.
    }
    void setHighlighted(bool selected) {
        selected_ = selected;
        applyColors();
    }
    quint64 id;
    std::function<void(QPointF)> moved;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override
    {
        if (change == ItemPositionChange)
            return clampedNodePosition(*this, value.toPointF(), view_);
        if (change == ItemPositionHasChanged && moved) moved(pos());
        return QGraphicsItemGroup::itemChange(change, value);
    }
private:
    void applyColors()
    {
        const QColor outline = !joined_ ? QColor("#a52d32")
            : complete_ ? QColor("#267343") : QColor("#9b7b16");
        const QColor fill = !joined_ ? QColor("#dc4c52")
            : complete_ ? QColor("#49b76b") : QColor("#f0ce4e");
        circle_->setPen(QPen(selected_ ? QColor("#287bff") : outline, selected_ ? 4.0 : 1.5));
        circle_->setBrush(fill);
    }
    const QGraphicsView& view_;
    QGraphicsEllipseItem* circle_;
    bool initiallyJoined_;
    bool joined_ = false;
    bool complete_ = false;
    bool selected_ = false;
};

class TrackerNode : public QGraphicsPixmapItem {
public:
    TrackerNode(const QPixmap& pixmap, const QGraphicsView& view)
        : QGraphicsPixmapItem(pixmap), view_(view)
    {
        setOffset(-pixmap.width() / 2.0, -pixmap.height() / 2.0);
        setFlags(ItemIsMovable | ItemSendsGeometryChanges);
        setAcceptedMouseButtons(Qt::LeftButton);
        setCursor(Qt::OpenHandCursor);
    }
    std::function<void(QPointF)> moved;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override
    {
        if (change == ItemPositionChange)
            return clampedNodePosition(*this, value.toPointF(), view_);
        if (change == ItemPositionHasChanged && moved) moved(pos());
        return QGraphicsPixmapItem::itemChange(change, value);
    }
private:
    const QGraphicsView& view_;
};
SimulationView::SimulationView(QWidget* parent)
    : QGraphicsView(parent)
{
    setObjectName("simulationCanvas");
    setScene(new QGraphicsScene(this));
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setRenderHint(QPainter::Antialiasing);
    setBackgroundBrush(palette().brush(QPalette::Base));
    setFrameShape(QFrame::StyledPanel);
    setMinimumSize(280, 180);
    setAccessibleName(tr("Simulation canvas"));
    setMouseTracking(true);
    updateCanvasRect();
}

void SimulationView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    updateCanvasRect();
    if (!editingEnabled_) return;
    // Shrinking the viewport may bring an edge across an existing node.
    // Movement callbacks persist the corrected position, including during placement.
    for (auto* item : scene()->items()) {
        if (!item->parentItem())
            item->setPos(clampedNodePosition(*item, item->pos(), *this));
    }
}

void SimulationView::updateCanvasRect()
{
    // Unscaled scene units match viewport pixels. Centered coordinates keep the
    // origin stable across swarm switches without a fit/zoom transform.
    const QSizeF size = viewport()->size();
    scene()->setSceneRect(QRectF(-size.width() / 2, -size.height() / 2,
        size.width(), size.height()));
}

PeerNode* SimulationView::addPeerNode(const ScenarioPeer& peer)
{
    auto* node = new PeerNode(peer, *this);
    scene()->addItem(node);
    node->moved = [this, swarmId = swarmId_, id = peer.id](QPointF position) {
        if (peersMovable() && (!pendingPeer_ || pendingPeer_->id != id) && peerMoved)
            peerMoved(swarmId, id, position);
        if (!runtimeWires_.empty()) drawRuntimeLinks();
    };
    node->setPos(clampedNodePosition(*node, peer.position, *this));
    node->moved(node->pos());
    node->setFlag(QGraphicsItem::ItemIsMovable, peersMovable());
    if (!peersMovable()) node->unsetCursor();
    return node;
}

void SimulationView::showSwarm(const ScenarioSwarm* swarm)
{
    if (runtimeMenu_) runtimeMenu_->close();
    cancelPeerPlacement();
    selectPeer(std::nullopt);
    refreshRuntimeLinks({});
    scene()->clear();
    swarmId_ = swarm ? swarm->id : 0;
    if (swarm) {
        const QPixmap icon(QStringLiteral(":/resources/nodes/tracker.png"));
        auto* tracker = new TrackerNode(icon.scaled(64, 64,
            Qt::KeepAspectRatio, Qt::SmoothTransformation),
            *this);
        scene()->addItem(tracker);

        tracker->setToolTip(tr("Tracker - %1").arg(swarm->name));
        tracker->moved = [this, swarmId = swarm->id](QPointF position) {
            if (editingEnabled_ && trackerMoved) trackerMoved(swarmId, position);
        };
        tracker->setPos(clampedNodePosition(*tracker, swarm->trackerPosition, *this));
        tracker->moved(tracker->pos());
        tracker->setFlag(QGraphicsItem::ItemIsMovable, editingEnabled_);
        if (!editingEnabled_) tracker->unsetCursor();
        for (const auto& peer : swarm->peers) addPeerNode(peer);
    }

}

void SimulationView::beginPeerPlacement(const ScenarioPeer& peer)
{
    if (!editingEnabled_ || !swarmId_ || pendingPeer_) return;
    pendingPeer_ = peer;
    preview_ = addPeerNode(peer);
    preview_->setFlag(QGraphicsItem::ItemIsMovable, false);
    preview_->setAcceptedMouseButtons(Qt::NoButton);
    preview_->setOpacity(0.65);
    preview_->setZValue(1);
    viewport()->setCursor(Qt::CrossCursor);
    setFocus(Qt::OtherFocusReason);
    updatePreview(viewport()->mapFromGlobal(QCursor::pos()));
    if (placementChanged) placementChanged();
}

void SimulationView::cancelPeerPlacement()
{
    if (!pendingPeer_) return;
    delete preview_;
    preview_ = nullptr;
    pendingPeer_.reset();
    viewport()->unsetCursor();
    if (placementChanged) placementChanged();
}

void SimulationView::updatePreview(const QPoint& position)
{
    if (!preview_) return;
    preview_->setVisible(viewport()->rect().contains(position));
    preview_->setPos(mapToScene(position));
}

void SimulationView::mouseMoveEvent(QMouseEvent* event)
{
    if (pendingPeer_) {
        updatePreview(event->position().toPoint());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void SimulationView::mousePressEvent(QMouseEvent* event)
{
    suppressContextMenu_ = false;
    if (pendingPeer_) {
        if (event->button() == Qt::RightButton) {
            cancelPeerPlacement();
            suppressContextMenu_ = true;
        }
        else if (event->button() == Qt::LeftButton) {
            updatePreview(event->position().toPoint());
            auto peer = *pendingPeer_;
            peer.position = preview_->pos();
            preview_->setFlag(QGraphicsItem::ItemIsMovable, true);
            preview_->setAcceptedMouseButtons(Qt::LeftButton);
            preview_->setOpacity(1);
            preview_->setZValue(0);
            preview_ = nullptr;
            pendingPeer_.reset();
            viewport()->unsetCursor();
            if (peerPlaced) peerPlaced(swarmId_, peer);
            if (placementChanged) placementChanged();
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        auto* item = itemAt(event->position().toPoint());
        while (item && item->parentItem()) item = item->parentItem();
        if (!editingEnabled_) {
            if (const auto* wire = dynamic_cast<RuntimeWireItem*>(item)) {
                selectedLink_ = wire->link;
                drawRuntimeLinks();
                if (selectionChanged) selectionChanged();
                event->accept();
                return;
            }
        }
        auto* peer = dynamic_cast<PeerNode*>(item);
        selectPeer(peer ? std::optional<quint64>(peer->id) : std::nullopt);
    }
    QGraphicsView::mousePressEvent(event);
}

void SimulationView::keyPressEvent(QKeyEvent* event)
{
    if (pendingPeer_ && event->key() == Qt::Key_Escape) {
        cancelPeerPlacement();
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void SimulationView::contextMenuEvent(QContextMenuEvent* event)
{
    if (suppressContextMenu_) {
        suppressContextMenu_ = false;
        event->accept();
        return;
    }
    if (pendingPeer_) {
        cancelPeerPlacement();
        event->accept();
        return;
    }
    auto* item = itemAt(event->pos());
    while (item && item->parentItem()) item = item->parentItem();
    auto* node = dynamic_cast<PeerNode*>(item);
    if (!node) return;
    if (!editingEnabled_) {
        if (runtimeMenu_) runtimeMenu_->close();
        const auto membership = runtimeMembership ? runtimeMembership(swarmId_, node->id) : std::nullopt;
        auto* menu = new QMenu(this);
        runtimeMenu_ = menu;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        auto* action = menu->addAction(membership && *membership ? tr("Leave swarm") : tr("Join swarm"));
        action->setEnabled(membership.has_value());
        if (!membership) action->setToolTip(tr("This swarm is not part of the runtime session."));
        connect(action, &QAction::triggered, this,
            [this, swarm = swarmId_, peer = node->id, joined = membership && *membership] {
                if (!editingEnabled_ && swarmId_ == swarm && runtimeMembershipChanged)
                    runtimeMembershipChanged(swarm, peer, !joined);
            });
        // No nested event loop and no retained PeerNode pointer while playback runs.
        menu->popup(event->globalPos());
        event->accept();
        return;
    }
    QMenu menu(this);
    auto* membership = menu.addAction(node->initiallyJoined() ? tr("Leave Swarm") : tr("Join Swarm"));
    auto* remove = menu.addAction(tr("Remove Peer"));
    const auto* selected = menu.exec(event->globalPos());
    if (selected == membership) {
        const bool joined = !node->initiallyJoined();
        if (peerInitiallyJoinedChanged) peerInitiallyJoinedChanged(swarmId_, node->id, joined);
        node->setInitiallyJoined(joined);
    } else if (selected == remove) {
        if (selectedPeer_ == node->id) selectPeer(std::nullopt);
        if (peerRemoved) peerRemoved(swarmId_, node->id);
        delete node;
    }
    event->accept();
}

bool SimulationView::viewportEvent(QEvent* event)
{
    if (preview_) {
        if (event->type() == QEvent::Leave) preview_->hide();
        else if (event->type() == QEvent::Enter)
            updatePreview(viewport()->mapFromGlobal(QCursor::pos()));
    }
    return QGraphicsView::viewportEvent(event);
}

void SimulationView::setEditingEnabled(bool enabled)
{
    if (!enabled) cancelPeerPlacement();
    if (enabled) { selectedLink_.reset(); refreshRuntimeLinks({}); }
    if (runtimeMenu_) runtimeMenu_->close();
    editingEnabled_ = enabled;
    if (enabled) runtimePaused_ = false;
    updateInteraction();
}

void SimulationView::setRuntimePaused(bool paused)
{
    if (runtimePaused_ == paused) return;
    runtimePaused_ = paused;
    updateInteraction();
}

void SimulationView::updateInteraction()
{
    setInteractive(peersMovable());
    for (auto* item : scene()->items()) {
        if (!item->parentItem()) {
            const bool movable = dynamic_cast<PeerNode*>(item) ? peersMovable()
                : dynamic_cast<TrackerNode*>(item) && editingEnabled_;
            item->setFlag(QGraphicsItem::ItemIsMovable, movable);
            if (movable) item->setCursor(Qt::OpenHandCursor);
            else item->unsetCursor();
        }
    }
}

void SimulationView::selectPeer(std::optional<quint64> id)
{
    if (selectedPeer_ == id && !selectedLink_) return;
    selectedLink_.reset();
    refreshRuntimeLinks({});
    selectedPeer_ = id;
    for (auto* item : scene()->items())
        if (auto* peer = dynamic_cast<PeerNode*>(item)) peer->setHighlighted(id == peer->id);
    if (selectionChanged) selectionChanged();
}

void SimulationView::refreshPeerColors(const std::function<PeerInspection(quint64)>& inspect)
{
    for (auto* item : scene()->items()) {
        auto* node = dynamic_cast<PeerNode*>(item);
        if (!node || node == preview_) continue;
        const auto state = inspect(node->id);
        node->setVisualState(state.joined, state.complete());
    }
}

void SimulationView::refreshRuntimeLinks(std::vector<RuntimeWire> wires)
{
    if (editingEnabled_) wires.clear();
    if (runtimeWires_ == wires) return; // Rebuild only for changed roles/active IDs; dragging redraws geometry separately.
    runtimeWires_ = std::move(wires);
    if (selectedLink_ && std::none_of(runtimeWires_.begin(), runtimeWires_.end(), [&](const RuntimeWire& wire) {
        return std::min(wire.sender, wire.receiver) == selectedLink_->a
            && std::max(wire.sender, wire.receiver) == selectedLink_->b;
    })) selectedLink_.reset();
    drawRuntimeLinks();
}

void SimulationView::drawRuntimeLinks()
{
    for (auto* item : wireItems_) delete item;
    wireItems_.clear();
    if (!selectedPeer_ || editingEnabled_) return;
    std::map<quint64, QPointF> positions;
    for (auto* item : scene()->items())
        if (const auto* node = dynamic_cast<PeerNode*>(item)) positions.emplace(node->id, node->scenePos());
    for (const auto& wire : runtimeWires_) {
        if (wire.sender != *selectedPeer_ && wire.receiver != *selectedPeer_) continue;
        if (!positions.contains(wire.sender) || !positions.contains(wire.receiver)) continue;
        const auto from = positions.at(wire.sender), to = positions.at(wire.receiver);
        const auto delta = to - from;
        const auto length = std::hypot(delta.x(), delta.y());
        // Overlapping circles have no visible segment between their boundaries.
        constexpr qreal radius = 18, offset = 4;
        const qreal inset = std::sqrt(radius * radius - offset * offset);
        if (length <= 2 * inset) continue;
        const auto unit = delta / length;
        const QPointF normal(-unit.y(), unit.x());
        // Reversal also reverses the normal, separating the return wire.
        const auto start = from + unit * inset + normal * offset;
        const auto end = to - unit * inset + normal * offset;
        const qreal arrow = qMin(qreal(8), (length - 2 * inset) / 2);
        QPainterPath path(start);
        path.lineTo(end);
        path.moveTo(end - unit * arrow + normal * (arrow / 2));
        path.lineTo(end);
        path.lineTo(end - unit * arrow - normal * (arrow / 2));
        const QColor color = wire.role == RuntimeWireRole::Choked ? QColor("#666666")
            : wire.role == RuntimeWireRole::Optimistic ? QColor("#287bff") : QColor("#299447");
        const RuntimeLinkSelection link{std::min(wire.sender, wire.receiver), std::max(wire.sender, wire.receiver)};
        auto* item = new RuntimeWireItem(path, link);
        item->setPen(QPen(color, selectedLink_ == link ? 3.5 : 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        item->setToolTip(tr("Link %1 - %2").arg(link.a).arg(link.b));
        scene()->addItem(item);
        if (wire.active) {
            const auto pixmap = QPixmap(wire.activeIcon).scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            auto* packet = new QGraphicsPixmapItem(pixmap, item);
            packet->setOffset(-pixmap.width() / 2.0, -pixmap.height() / 2.0);
            packet->setShapeMode(QGraphicsPixmapItem::BoundingRectShape);
            packet->setPos((from + to) / 2 + normal * 13);
            packet->setToolTip(tr("Active transmission %1: %2 -> %3").arg(*wire.active).arg(wire.sender).arg(wire.receiver));
            packet->setData(0, QVariant::fromValue<qulonglong>(*wire.active));
            packet->setAcceptedMouseButtons(Qt::NoButton);
        }
        item->setZValue(-1);
        item->setAcceptedMouseButtons(Qt::NoButton);
        wireItems_.push_back(item);
    }
}
