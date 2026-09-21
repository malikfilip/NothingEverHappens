#pragma once

#include <QMainWindow>
#include <QPointer>
#include "ScenarioSwarm.hpp"
#include "ScenarioSettings.hpp"
#include "RuntimePump.hpp"
#include "simulator/PeerCompletedEvent.hpp"
#include <vector>
#include <memory>
#include <deque>

class QStackedWidget;
class QComboBox;
class QAction;
class QLabel;
class QTableView;
class QStandardItemModel;
class SimulationView;
class RuntimeSession;
class QPushButton;
class PieceBitmapDialog;
class QMessageBox;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void createToolbar();
    void showSettings();
    void importScenario();
    void exportScenario();
    void play();
    void stopSimulation();
    void updateRuntimeControls();
    void queueCompletion(simulator::PeerId peer, simulator::SwarmId swarm);
    void presentCompletions();
    void showNextCompletion();
    void addSwarm();
    void addPeer();
    ScenarioSwarm* findSwarm(quint64 id);
    void showSwarmInfo();
    void editSwarm(quint64 id);
    void refreshInspector();
    QWidget* createInspector();
    QWidget* createEventLog();
    QWidget* createMessageFilter();

    std::vector<ScenarioSwarm> swarms_;
    ScenarioSettings settings_;
    std::deque<QString> completionMessages_;
    QTimer completionTimer_;
    QPointer<QMessageBox> completionPopup_;
    bool presentingCompletions_ = false;
    bool resumeAfterCompletions_ = false;
    QAction* settingsAction_ = nullptr;
    QAction* importAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    std::unique_ptr<RuntimeSession> runtime_;
    RuntimePump pump_; // Destroyed before the session.
    QAction* pauseAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* nextEventAction_ = nullptr;
    QLabel* simulationTime_ = nullptr;
    QTableView* eventLog_ = nullptr;
    QStandardItemModel* eventLogModel_ = nullptr;
    quint64 scenarioSeed_ = 0;
    QComboBox* swarmSelector_ = nullptr;
    QAction* swarmInfo_ = nullptr;
    QAction* addPeerAction_ = nullptr;
    QAction* addSwarmAction_ = nullptr;
    QAction* playAction_ = nullptr;
    quint64 nextPeerId_ = 1;
    SimulationView* canvas_ = nullptr;
    QStackedWidget* inspectorContents_ = nullptr;
    QLabel* inspectorDetails_ = nullptr;
    QPushButton* viewPieces_ = nullptr;
    PieceBitmapDialog* piecesDialog_ = nullptr;
};
