#pragma once

#include <QMainWindow>
#include "ScenarioSwarm.hpp"
#include "RuntimePump.hpp"
#include <vector>
#include <memory>

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

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void createToolbar();
    void play();
    void updateRuntimeControls();
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
    std::unique_ptr<RuntimeSession> runtime_;
    RuntimePump pump_; // Destroyed before the session.
    QAction* pauseAction_ = nullptr;
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
