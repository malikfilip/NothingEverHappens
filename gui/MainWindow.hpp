#pragma once

#include <QMainWindow>
#include "ScenarioSwarm.hpp"
#include <vector>

class QStackedWidget;
class QComboBox;
class QAction;
class SimulationView;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void createToolbar();
    void addSwarm();
    void addPeer();
    ScenarioSwarm* findSwarm(quint64 id);
    void showSwarmInfo();
    QWidget* createInspector();
    QWidget* createEventLog();
    QWidget* createMessageFilter();

    std::vector<ScenarioSwarm> swarms_;
    QComboBox* swarmSelector_ = nullptr;
    QAction* swarmInfo_ = nullptr;
    QAction* addPeerAction_ = nullptr;
    quint64 nextPeerId_ = 1;
    SimulationView* canvas_ = nullptr;
    QStackedWidget* inspectorContents_ = nullptr;
};
