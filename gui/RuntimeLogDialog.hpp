#pragma once
#include <memory>
#include <QDialog>
class RuntimeEventLog;
// No dialog for non-message rows or while playback is running.
std::unique_ptr<QDialog> createRuntimeLogDialog(const RuntimeEventLog& log, int row,
    bool paused, QWidget* parent = nullptr);

namespace simulator { struct ActiveTransmission; }
std::unique_ptr<QDialog> createActiveTransmissionDialog(const simulator::ActiveTransmission& active,
    double simulationTime, bool paused, QWidget* parent = nullptr);
