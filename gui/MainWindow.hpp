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
    void showSwarmInfo();
    QWidget* createInspector();
    QWidget* createEventLog();
    QWidget* createMessageFilter();

    std::vector<ScenarioSwarm> swarms_;
    QComboBox* swarmSelector_ = nullptr;
    QAction* swarmInfo_ = nullptr;
    SimulationView* canvas_ = nullptr;
    QStackedWidget* inspectorContents_ = nullptr;
};
