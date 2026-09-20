#pragma once

#include <QPointF>
#include <QString>
#include <QtGlobal>
#include <cstdint>
#include <optional>
#include <vector>

// Owned by exactly one ScenarioSwarm. Configuration, not live engine state.
struct ScenarioPeer {
    enum class Role { Leecher, Seeder };
    quint64 id = 0;
    QString name;
    Role initialRole = Role::Leecher;
    bool initiallyJoined = false; // Scenario configuration, not runtime membership.
    quint64 initialPieceCount = 0;
    // nullopt means not generated yet. Piece i is bit (0x80 >> (i % 8))
    // in byte i / 8; unused trailing bits are zero. Retained across GUI refreshes.
    std::optional<std::vector<std::uint8_t>> initialBitfield;
    quint64 uploadBytesPerSecond = 0;
    quint64 downloadBytesPerSecond = 0;
    QString uploadDisplayValue;
    QString uploadDisplayUnit;
    QString downloadDisplayValue;
    QString downloadDisplayUnit;
    QPointF position;
};
