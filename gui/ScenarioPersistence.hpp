#pragma once
#include "ScenarioSwarm.hpp"
#include "ScenarioSettings.hpp"
#include <QByteArray>

// Editable project only. No engine objects, timers, or transient GUI state.
struct ScenarioProject {
    std::vector<ScenarioSwarm> swarms;
    ScenarioSettings settings;
    quint64 seed = 0;
};

namespace ScenarioPersistence {
// Throw std::invalid_argument for invalid data; file errors throw std::runtime_error.
// Load constructs and validates a new project, never mutating the caller's project.
QByteArray toJson(const ScenarioProject& project);
ScenarioProject fromJson(const QByteArray& json);
void save(const QString& path, const ScenarioProject& project);
ScenarioProject load(const QString& path);
}
