#pragma once

#include <QGraphicsView>

struct ScenarioSwarm;

// Presentation-only canvas, independent of the simulator engine.
class SimulationView : public QGraphicsView {
public:
    explicit SimulationView(QWidget* parent = nullptr);
    void showSwarm(const ScenarioSwarm* swarm);
};
