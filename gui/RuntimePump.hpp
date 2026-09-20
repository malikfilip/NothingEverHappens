#pragma once
#include "simulator/Simulation.hpp"
#include <QTimer>
#include <QElapsedTimer>
#include "PlaybackClock.hpp"
#include <functional>
#include <exception>

// Qt Core only. Simulation and callback targets must outlive the pump.
class RuntimePump : public QObject {
public:
    enum class State { Edit, Running, Paused };
    RuntimePump() {
        wallClock_.start();
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    }
    State state() const { return state_; }
    double playbackSpeed() const { return pacing_.speed(); }
    void setPlaybackSpeed(double speed) {
        pacing_.setSpeed(speed, simulation_ ? simulation_->currentTime() : 0, now());
        if (state_ == State::Running) schedule();
    }
    void start(simulator::Simulation& simulation) {
        if (state_ != State::Edit) return;
        simulation_ = &simulation;
        pacing_.reset(simulation.currentTime(), now());
        resume();
    }
    void resume() {
        if (!simulation_ || state_ == State::Running) return;
        pacing_.resume(now());
        state_ = State::Running;
        if (stateChanged) stateChanged();
        if (state_ == State::Running) schedule();
    }
    void pause() {
        if (state_ != State::Running) return;
        timer_.stop();
        pacing_.pause(now());
        state_ = State::Paused;
        if (stateChanged) stateChanged();
    }
    std::function<void()> stateChanged;
    std::function<void()> stepped;
    std::function<void(const char*)> failed;
private:
    // 64 separate callbacks followed by a 1 ms UI breathing interval.
    // Scheduling policy only: never scales simulation time or batches steps.
    static constexpr unsigned callbacksPerYield = 64;
    double now() const { return static_cast<double>(wallClock_.nsecsElapsed()) / 1e9; }
    int delay() const {
        const auto next = simulation_->nextEventTime();
        return next ? pacing_.delayMilliseconds(*next, simulation_->currentTime(), now()) : 0;
    }
    void schedule(int yield = 0) { timer_.start(std::max(yield, delay())); }
    void tick() {
        if (state_ != State::Running) return;
        // Recheck after every wakeup, including long-gap slices.
        if (const int wait = delay(); wait > 0) { timer_.start(wait); return; }
        try {
            const bool executed = simulation_->step();
            if (stepped) stepped();
            if (!executed) pause();
        } catch (const std::exception& error) {
            pause();
            if (failed) failed(error.what());
            return;
        }
        if (state_ == State::Running)
            schedule(++callbacks_ % callbacksPerYield == 0 ? 1 : 0);
    }
    QElapsedTimer wallClock_;
    PlaybackClock pacing_;
    QTimer timer_;
    simulator::Simulation* simulation_ = nullptr;
    State state_ = State::Edit;
    unsigned callbacks_ = 0;
};
