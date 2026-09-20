#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

// GUI scheduling only. Wall times are monotonic seconds supplied by the caller.
// This virtual position never changes the engine's logical time.
class PlaybackClock {
public:
    double speed() const { return speed_; } // Zero means Max.
    void resume(double now) { wallAnchor_ = now; running_ = true; }
    void pause(double now) {
        position_ = position(now);
        wallAnchor_ = now;
        running_ = false;
    }
    void reset(double simulationTime, double now) {
        position_ = simulationTime;
        wallAnchor_ = now;
    }
    void setSpeed(double speed, double simulationTime, double now) {
        if (!std::isfinite(speed) || speed < 0) throw std::invalid_argument("Invalid playback speed");
        // Leaving Max starts a new paced timeline at the actual engine time.
        position_ = speed_ == 0 ? simulationTime : position(now);
        wallAnchor_ = now;
        speed_ = speed;
    }
    int delayMilliseconds(double next, double current, double now) const {
        if (speed_ == 0 || next <= current) return 0;
        const double remaining = (next - position(now)) / speed_;
        if (remaining <= 0) return 0;
        // Bound before conversion; long gaps are rechecked in one-second slices.
        // Ceil prevents early execution; absolute anchors prevent cumulative drift.
        if (!std::isfinite(remaining) || remaining >= 1) return 1000;
        return static_cast<int>(std::ceil(remaining * 1000));
    }
private:
    double position(double now) const {
        return position_ + (running_ ? std::max(0.0, now - wallAnchor_) * speed_ : 0.0);
    }
    double speed_ = 1;
    double position_ = 0;
    double wallAnchor_ = 0;
    bool running_ = false;
};
