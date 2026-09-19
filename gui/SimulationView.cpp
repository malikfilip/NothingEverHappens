#include "SimulationView.hpp"
#include "ScenarioSwarm.hpp"

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
#include <QPalette>
#include <QResizeEvent>

namespace {
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
        : id(peer.id), view_(view), role_(peer.initialRole), initiallyJoined_(peer.initiallyJoined)
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
        const QColor outline = !initiallyJoined_ ? QColor("#a52d32")
            : role_ == ScenarioPeer::Role::Seeder ? QColor("#267343") : QColor("#9b7b16");
        const QColor fill = !initiallyJoined_ ? QColor("#dc4c52")
            : role_ == ScenarioPeer::Role::Seeder ? QColor("#49b76b") : QColor("#f0ce4e");
        circle_->setPen(QPen(outline, 1.5));
        circle_->setBrush(fill);
    }
    const QGraphicsView& view_;
    QGraphicsEllipseItem* circle_;
    ScenarioPeer::Role role_;
    bool initiallyJoined_;
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
        if ((!pendingPeer_ || pendingPeer_->id != id) && peerMoved)
            peerMoved(swarmId, id, position);
    };
    node->setPos(clampedNodePosition(*node, peer.position, *this));
    node->moved(node->pos());
    return node;
}

void SimulationView::showSwarm(const ScenarioSwarm* swarm)
{
    cancelPeerPlacement();
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
            if (trackerMoved) trackerMoved(swarmId, position);
        };
        tracker->setPos(clampedNodePosition(*tracker, swarm->trackerPosition, *this));
        tracker->moved(tracker->pos());
        for (const auto& peer : swarm->peers) addPeerNode(peer);
    }

}

void SimulationView::beginPeerPlacement(const ScenarioPeer& peer)
{
    if (!swarmId_ || pendingPeer_) return;
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
    QMenu menu(this);
    auto* membership = menu.addAction(node->initiallyJoined() ? tr("Leave Swarm") : tr("Join Swarm"));
    auto* remove = menu.addAction(tr("Remove Peer"));
    const auto* selected = menu.exec(event->globalPos());
    if (selected == membership) {
        const bool joined = !node->initiallyJoined();
        if (peerInitiallyJoinedChanged) peerInitiallyJoinedChanged(swarmId_, node->id, joined);
        node->setInitiallyJoined(joined);
    } else if (selected == remove) {
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
