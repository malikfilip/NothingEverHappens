#pragma once

#include "ScenarioSettings.hpp"
#include <QDialog>

class QDoubleSpinBox;
class QLabel;

class SimulationSettingsDialog : public QDialog {
public:
    explicit SimulationSettingsDialog(const ScenarioSettings& settings, QWidget* parent = nullptr);
    ScenarioSettings settings() const;
    void accept() override;

private:
    QDoubleSpinBox* regular_;
    QDoubleSpinBox* optimistic_;
    QLabel* error_;
};
