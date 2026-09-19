#pragma once

#include <QString>
#include <QtGlobal>

// Scenario metadata owned only by the GUI; no simulator objects are created.
struct ScenarioSwarm {
    enum class Mode { File, Virtual };

    quint64 id = 0;
    QString name;
    Mode mode = Mode::File;
    QString filePath;
    quint64 totalSizeBytes = 0;
    quint64 pieceSizeBytes = 0;
    quint64 pieceCount = 0;

    // Presentation metadata; byte fields above remain authoritative for calculations.
    QString virtualSizeDisplayValue;
    QString virtualSizeDisplayUnit;
    QString pieceSizeDisplayValue;
    QString pieceSizeDisplayUnit;
};
