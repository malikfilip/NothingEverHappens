#pragma once

#include <cmath>

namespace simulator {
struct BitTorrentSettings {
    double regularRechokeInterval = 10.0;
    double optimisticUnchokeInterval = 30.0;

    const char* validationError() const {
        if (!std::isfinite(regularRechokeInterval) || regularRechokeInterval <= 0
            || !std::isfinite(optimisticUnchokeInterval) || optimisticUnchokeInterval <= 0)
            return "Both choking intervals must be finite and greater than zero.";
        if (optimisticUnchokeInterval < regularRechokeInterval)
            return "The optimistic unchoke interval must be at least the regular rechoke interval.";
        return nullptr;
    }
};

}
