#include "SimulationView.hpp"
#include "ScenarioSwarm.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>

class PeerNode : public QGraphicsItem {
public:
    PeerNode(const ScenarioPeer& peer, const QRectF& bounds)
        : id(peer.id), name_(peer.name), bounds_(bounds), font_(QApplication::font())
    {
        const QFontMetricsF metrics(font_);
        label_ = metrics.elidedText(name_, Qt::ElideMiddle, 140);
        const qreal width = qMax(36.0, metrics.horizontalAdvance(label_));
        rect_ = QRectF(-width / 2 - 2, -18, width + 4, 40 + metrics.height());
        setFlags(ItemIsMovable | ItemSendsGeometryChanges);
        setAcceptedMouseButtons(Qt::LeftButton);
        setCursor(Qt::OpenHandCursor);
        setToolTip(name_);
        setPos(peer.position);
    }
    QRectF boundingRect() const override { return rect_; }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(QColor("#a52d32"), 1.5));
        painter->setBrush(QColor("#dc4c52"));
        painter->drawEllipse(QRectF(-16, -16, 32, 32));
        painter->setFont(font_);
        painter->setPen(QApplication::palette().color(QPalette::Text));
        painter->drawText(QRectF(rect_.left(), 21, rect_.width(), rect_.height() - 39),
            Qt::AlignHCenter | Qt::AlignTop, label_);
    }
    quint64 id;
    std::function<void(QPointF)> moved;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override
    {
        if (change == ItemPositionChange) {
            const auto point = value.toPointF();
            return QPointF(qBound(bounds_.left() - rect_.left(), point.x(), bounds_.right() - rect_.right()),
                qBound(bounds_.top() - rect_.top(), point.y(), bounds_.bottom() - rect_.bottom()));
        }
        if (change == ItemPositionHasChanged && moved) moved(pos());
        return QGraphicsItem::itemChange(change, value);
    }
private:
    QString name_;
    QString label_;
    QRectF bounds_;
    QRectF rect_;
    QFont font_;
};

SimulationView::SimulationView(QWidget* parent)
    : QGraphicsView(parent)
{
    setObjectName("simulationCanvas");
    setScene(new QGraphicsScene(this));
    // Fixed editing coordinates preserve positions across window resizing and swarm switches.
    scene()->setSceneRect(-600, -400, 1200, 800);
    setRenderHint(QPainter::Antialiasing);
    setBackgroundBrush(palette().brush(QPalette::Base));
    setFrameShape(QFrame::StyledPanel);
    setMinimumSize(280, 180);
    setAccessibleName(tr("Simulation canvas"));
    setMouseTracking(true);
}

PeerNode* SimulationView::addPeerNode(const ScenarioPeer& peer)
{
    auto* node = new PeerNode(peer, scene()->sceneRect().adjusted(8, 8, -8, -8));
    scene()->addItem(node);
    node->moved = [this, swarmId = swarmId_, id = peer.id](QPointF position) {
        if (!pendingPeer_ && peerMoved) peerMoved(swarmId, id, position);
    };
    return node;
}

void SimulationView::showSwarm(const ScenarioSwarm* swarm)
{
    cancelPeerPlacement();
    scene()->clear();
    swarmId_ = swarm ? swarm->id : 0;
    if (swarm) {
        const QPixmap icon(QStringLiteral(":/resources/nodes/tracker.png"));
        auto* tracker = scene()->addPixmap(icon.scaled(64, 64,
            Qt::KeepAspectRatio, Qt::SmoothTransformation));
        tracker->setOffset(-tracker->pixmap().width() / 2.0, -tracker->pixmap().height() / 2.0);
        tracker->setToolTip(tr("Tracker - %1").arg(swarm->name));
        for (const auto& peer : swarm->peers) addPeerNode(peer);
    }
    centerOn(0, 0);
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
    auto* node = dynamic_cast<PeerNode*>(itemAt(event->pos()));
    if (!node) return;
    QMenu menu(this);
    auto* remove = menu.addAction(tr("Remove Peer"));
    if (menu.exec(event->globalPos()) == remove) {
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
