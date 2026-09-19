#pragma once

#include "ScenarioPeer.hpp"
#include <QGraphicsView>
#include <functional>
#include <optional>

struct ScenarioSwarm;
class PeerNode;

// Presentation-only canvas. Callbacks identify scenario records without retaining pointers.
class SimulationView : public QGraphicsView {
public:
    explicit SimulationView(QWidget* parent = nullptr);
    void showSwarm(const ScenarioSwarm* swarm);
    void beginPeerPlacement(const ScenarioPeer& peer);
    void cancelPeerPlacement();
    bool isPlacingPeer() const { return pendingPeer_.has_value(); }

    std::function<void(quint64, const ScenarioPeer&)> peerPlaced;
    std::function<void(quint64, quint64, QPointF)> peerMoved;
    std::function<void(quint64, quint64)> peerRemoved;
    std::function<void(quint64, quint64, bool)> peerInitiallyJoinedChanged;
    std::function<void(quint64, QPointF)> trackerMoved;
    std::function<void()> placementChanged;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    PeerNode* addPeerNode(const ScenarioPeer& peer);
    void updatePreview(const QPoint& viewportPosition);
    void updateCanvasRect();
    quint64 swarmId_ = 0;
    std::optional<ScenarioPeer> pendingPeer_;
    PeerNode* preview_ = nullptr;
    bool suppressContextMenu_ = false;
};
