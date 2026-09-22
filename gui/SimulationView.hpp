#pragma once

#include "ScenarioPeer.hpp"
#include "RuntimeLinks.hpp"
#include <QGraphicsView>
#include <QPointer>
class QMenu;
#include <functional>
#include <optional>

struct ScenarioSwarm;
class PeerNode;
struct PeerInspection;

// Presentation-only canvas. Callbacks identify scenario records without retaining pointers.
class SimulationView : public QGraphicsView {
public:
    explicit SimulationView(QWidget* parent = nullptr);
    void showSwarm(const ScenarioSwarm* swarm);
    void setEditingEnabled(bool enabled);
    void setRuntimePaused(bool paused);
    void refreshPeerColors(const std::function<PeerInspection(quint64)>& inspect);
    void refreshRuntimeLinks(std::vector<RuntimeWire> wires);
    void beginPeerPlacement(const ScenarioPeer& peer);
    void cancelPeerPlacement();
    bool isPlacingPeer() const { return pendingPeer_.has_value(); }

    std::optional<quint64> selectedPeer() const { return selectedPeer_; }
    std::optional<RuntimeLinkSelection> selectedLink() const { return selectedLink_; }
    quint64 shownSwarm() const { return swarmId_; }
    std::function<void()> selectionChanged;
    std::function<void(quint64, const ScenarioPeer&)> peerPlaced;
    std::function<void(quint64, quint64, QPointF)> peerMoved;
    std::function<void(quint64, quint64)> peerRemoved;
    std::function<void(quint64, quint64, bool)> peerInitiallyJoinedChanged;
    std::function<std::optional<bool>(quint64, quint64)> runtimeMembership;
    std::function<void(quint64, quint64, bool)> runtimeMembershipChanged;
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
    void drawRuntimeLinks();
    void updateInteraction();
    bool peersMovable() const { return editingEnabled_ || runtimePaused_; }
    QPointer<QMenu> runtimeMenu_;
    bool runtimePaused_ = false;
    std::vector<RuntimeWire> runtimeWires_;
    std::vector<QGraphicsItem*> wireItems_;
    void selectPeer(std::optional<quint64> id);
    std::optional<quint64> selectedPeer_;
    std::optional<RuntimeLinkSelection> selectedLink_;
    PeerNode* addPeerNode(const ScenarioPeer& peer);
    void updatePreview(const QPoint& viewportPosition);
    void updateCanvasRect();
    quint64 swarmId_ = 0;
    std::optional<ScenarioPeer> pendingPeer_;
    PeerNode* preview_ = nullptr;
    bool suppressContextMenu_ = false;
    bool editingEnabled_ = true;
};
