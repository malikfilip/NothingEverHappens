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
        displayTimer_.setTimerType(Qt::PreciseTimer);
        displayTimer_.setInterval(16);
        connect(&displayTimer_, &QTimer::timeout, this, [this] {
            if (state_ == State::Running && playbackChanged) playbackChanged();
        });
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    }
    State state() const { return state_; }
    double playbackTime() const {
        if (!simulation_) return 0;
        const double current = simulation_->currentTime();
        const auto next = simulation_->nextEventTime();
        if (pacing_.speed() == 0 || !next) return current;
        return std::clamp(pacing_.position(now()), current, *next);
    }
    void nextEvent() {
        if (state_ != State::Paused || !simulation_) return;
        try {
            simulation_->step();
            pacing_.reset(simulation_->currentTime(), now());
            if (stepped) stepped();
        } catch (const std::exception& error) {
            pacing_.reset(simulation_->currentTime(), now());
            if (failed) failed(error.what());
        }
        if (playbackChanged) playbackChanged();
        if (stateChanged) stateChanged();
    }
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
    // Detach before the owner destroys Simulation. Safe to call again in EDIT.
    void stop() {
        timer_.stop();
        displayTimer_.stop();
        pacing_.pause(now());
        pacing_.reset(0, now());
        simulation_ = nullptr;
        callbacks_ = 0;
        state_ = State::Edit;
        if (stateChanged) stateChanged();
        if (playbackChanged) playbackChanged();
    }
    void resume() {
        if (!simulation_ || state_ == State::Running) return;
        pacing_.resume(now());
        state_ = State::Running;
        displayTimer_.start();
        if (stateChanged) stateChanged();
        if (state_ == State::Running) schedule();
    }
    void pause() {
        if (state_ != State::Running) return;
        timer_.stop();
        displayTimer_.stop();
        pacing_.pause(now());
        pacing_.reset(playbackTime(), now());
        state_ = State::Paused;
        if (stateChanged) stateChanged();
    }
    std::function<void()> stateChanged;
    std::function<void()> stepped;
    std::function<void()> playbackChanged;
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
        // Event deadlines and display refreshes share one PlaybackClock.
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
    QTimer displayTimer_; // Presentation refresh only; never steps the engine.
    simulator::Simulation* simulation_ = nullptr;
    State state_ = State::Edit;
    unsigned callbacks_ = 0;
};
