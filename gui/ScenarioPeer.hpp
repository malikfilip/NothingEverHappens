#pragma once

#include <QPointF>
#include <QString>
#include <QtGlobal>

// Owned by exactly one ScenarioSwarm. No engine state or piece indices.
struct ScenarioPeer {
    enum class Role { Leecher, Seeder };
    quint64 id = 0;
    QString name;
    Role initialRole = Role::Leecher;
    quint64 initialPieceCount = 0;
    quint64 uploadBytesPerSecond = 0;
    quint64 downloadBytesPerSecond = 0;
    QString uploadDisplayValue;
    QString uploadDisplayUnit;
    QString downloadDisplayValue;
    QString downloadDisplayUnit;
    QPointF position;
};
