#include "RuntimePump.hpp"
#include "RuntimeSession.hpp"
#include "PeerInspection.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Runtime pump check failed at line " + std::to_string(where.line()));
}
class Action : public simulator::Event {
public:
    Action(double time, std::function<void()> body) : Event(time), body_(std::move(body)) {}
    void execute() override { body_(); }
private:
    std::function<void()> body_;
};
void cycle() { QCoreApplication::processEvents(); }
void settle() {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
}
void pacingCalculations() {
    for (double speed : {0.5, 1.0, 2.0, 5.0, 10.0}) {
        PlaybackClock clock;
        clock.setSpeed(speed, 0, 0);
        clock.resume(0);
        // An exact half real second at each speed, independent of engine time.
        check(clock.delayMilliseconds(speed * 0.5, 0, 0) == 500);
        check(clock.delayMilliseconds(speed * 0.5, 0, 0.25) == 250);
        check(clock.delayMilliseconds(speed * 0.5, 0, 0.5) == 0);
        check(clock.delayMilliseconds(0, 0, 0) == 0);
        check(clock.delayMilliseconds(1e-9, 0, 0) == 1);
        check(clock.delayMilliseconds(std::numeric_limits<double>::max(), 0, 0) == 1000);
    }
    PlaybackClock clock;
    check(clock.speed() == 1);
    clock.resume(0);
    clock.pause(0.25);
    check(clock.delayMilliseconds(1, 0, 100) == 750);
    clock.setSpeed(2, 0, 100); // Paused changes don't consume waiting time.
    clock.resume(200);
    check(clock.delayMilliseconds(1, 0, 200) == 375);
    clock.setSpeed(0.5, 0, 200.125); // Position is now 0.5 simulated seconds.
    check(clock.delayMilliseconds(1, 0, 200.125) == 1000);
    check(clock.delayMilliseconds(1, 0, 201.125) == 0);
    // Late callbacks consume existing timeline credit, not a fresh per-event gap.
    check(clock.delayMilliseconds(1.125, 1, 201.5) == 0);
    clock.setSpeed(0, 1, 201.5);
    check(clock.delayMilliseconds(1e100, 1, 201.5) == 0);
    clock.setSpeed(1, 50, 202);
    check(clock.delayMilliseconds(50.5, 50, 202) == 500);
    clock.pause(202.25);
    clock.resume(300);
    check(clock.delayMilliseconds(50.5, 50, 300) == 250);
}
void delayedPump() {
    simulator::Simulation simulation;
    RuntimePump pump;
    int executed = 0;
    simulation.schedule(std::make_unique<Action>(1e100, [&] { ++executed; }));
    check(pump.playbackSpeed() == 1);
    pump.start(simulation);
    settle();
    check(executed == 0 && simulation.currentTime() == 0);
    pump.pause();
    pump.setPlaybackSpeed(10);
    settle();
    check(executed == 0 && pump.state() == RuntimePump::State::Paused);
    pump.resume();
    settle();
    check(executed == 0);
    pump.setPlaybackSpeed(0); // Replaces the pending long timer immediately.
    settle();
    check(executed == 1 && simulation.currentTime() == 1e100);
    check(pump.state() == RuntimePump::State::Paused);

    simulator::Simulation paced;
    RuntimePump pacedPump;
    int equalTime = 0;
    paced.schedule(std::make_unique<Action>(0.02, [&] { ++equalTime; }));
    paced.schedule(std::make_unique<Action>(0.02, [&] { ++equalTime; }));
    paced.schedule(std::make_unique<Action>(0.020000001, [&] { ++equalTime; }));
    pacedPump.setPlaybackSpeed(0);
    pacedPump.start(paced);
    pacedPump.setPlaybackSpeed(1); // Leaving Max cancels its queued immediate step.
    check(paced.currentTime() == 0);
    QElapsedTimer elapsed;
    elapsed.start();
    QEventLoop loop;
    pacedPump.stateChanged = [&] { if (pacedPump.state() == RuntimePump::State::Paused) loop.quit(); };
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    check(equalTime == 3 && elapsed.elapsed() >= 20);
    check(pacedPump.state() == RuntimePump::State::Paused);
}
void completionAppearanceState()
{
    ScenarioSwarm swarm;
    swarm.id = 1;
    swarm.name = QStringLiteral("Color refresh");
    swarm.totalSizeBytes = swarm.pieceSizeBytes = 1024;
    swarm.pieceCount = 1;
    ScenarioPeer seed;
    seed.id = 1;
    seed.name = QStringLiteral("Seed");
    seed.initiallyJoined = true;
    seed.initialRole = ScenarioPeer::Role::Seeder;
    seed.initialPieceCount = 1;
    seed.initialBitfield = std::vector<std::uint8_t>{0x80};
    seed.uploadBytesPerSecond = seed.downloadBytesPerSecond = 1024 * 1024;
    ScenarioPeer leecher = seed;
    leecher.id = 2;
    leecher.name = QStringLiteral("Leecher");
    leecher.initialRole = ScenarioPeer::Role::Leecher;
    // EDIT appearance depends on ownership, even if the configured role says Leecher.
    check(inspectPeer(swarm, leecher, nullptr).complete());
    leecher.initialPieceCount = 0;
    leecher.initialBitfield = std::vector<std::uint8_t>{0};
    check(!inspectPeer(swarm, leecher, nullptr).complete());
    auto inactive = seed;
    inactive.initiallyJoined = false;
    check(!inspectPeer(swarm, inactive, nullptr).joined);
    swarm.peers = {seed, leecher};
    std::vector<ScenarioSwarm> scenario{swarm};
    auto session = RuntimeSession::create(scenario, {0, 0, 100, 100});
    const auto initialSeed = inspectPeer(scenario[0], scenario[0].peers[0], session.get());
    check(initialSeed.joined && initialSeed.complete()); // Green before the first step.
    const auto initialLeecher = inspectPeer(scenario[0], scenario[0].peers[1], session.get());
    check(initialLeecher.joined && !initialLeecher.complete()); // Yellow.
    RuntimePump pump;
    pump.setPlaybackSpeed(0);
    bool completed = false;
    pump.stepped = [&] {
        const auto current = inspectPeer(scenario[0], scenario[0].peers[1], session.get());
        if (current.joined && current.complete()) {
            completed = true; // Same post-step snapshot used for the canvas brush.
            pump.pause();
        }
    };
    QEventLoop loop;
    pump.stateChanged = [&] { if (pump.state() == RuntimePump::State::Paused) loop.quit(); };
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    pump.start(session->simulation());
    loop.exec();
    check(completed && pump.state() == RuntimePump::State::Paused);
    const auto pausedTime = session->simulation().currentTime();
    settle();
    const auto paused = inspectPeer(scenario[0], scenario[0].peers[1], session.get());
    check(paused.joined && paused.complete() && session->simulation().currentTime() == pausedTime);
    check(scenario[0].peers[1].initialRole == ScenarioPeer::Role::Leecher
        && scenario[0].peers[1].initialPieceCount == 0); // No rewriting of scenario roles.
}
void recurringPauseResume() {
    ScenarioSwarm swarm;
    swarm.id = 1;
    swarm.name = QStringLiteral("Pump swarm");
    swarm.totalSizeBytes = 1024;
    swarm.pieceSizeBytes = 1024;
    swarm.pieceCount = 1;
    ScenarioPeer peer;
    peer.id = 1;
    peer.name = QStringLiteral("Pump peer");
    peer.initiallyJoined = true;
    peer.initialRole = ScenarioPeer::Role::Seeder;
    peer.initialPieceCount = 1;
    peer.uploadBytesPerSecond = peer.downloadBytesPerSecond = 1024;
    swarm.peers.push_back(peer);
    std::vector<ScenarioSwarm> scenario{swarm};
    auto session = RuntimeSession::create(scenario, {0, 0, 100, 100});
    auto* identity = session.get();
    auto& simulation = session->simulation();
    RuntimePump pump;
    pump.setPlaybackSpeed(0);
    int observations = 0, callbacks = 0;
    std::vector<double> times;
    simulation.setEventObserver([&](const simulator::Event& event) {
        ++observations;
        times.push_back(event.time());
    });
    pump.stepped = [&] {
        ++callbacks;
        if (callbacks == 70 || callbacks == 71) pump.pause();
    };
    check(pump.state() == RuntimePump::State::Edit);
    pump.resume(); cycle(); check(observations == 0);
    pump.start(simulation);
    pump.start(simulation); // Cannot replace or duplicate the running session.
    pump.resume();
    QEventLoop loop;
    pump.stateChanged = [&] { if (pump.state() == RuntimePump::State::Paused) loop.quit(); };
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    check(callbacks == 70 && observations == 70);
    check(pump.state() == RuntimePump::State::Paused);
    const auto pausedTime = simulation.currentTime();
    settle();
    check(callbacks == 70 && simulation.currentTime() == pausedTime);
    pump.resume();
    check(session.get() == identity && simulation.currentTime() == pausedTime);
    settle();
    check(callbacks == 71 && observations == 71 && simulation.currentTime() > pausedTime);
    check(times.front() == 0 && times.back() == simulation.currentTime());
}
void atomicOrderingEmptyAndFailure() {
    simulator::Simulation simulation;
    RuntimePump pump;
    pump.setPlaybackSpeed(0);
    int executed = 0, callbacks = 0;
    std::vector<int> order;
    simulation.setEventObserver([&](const simulator::Event&) { order.push_back(executed); });
    simulation.schedule(std::make_unique<Action>(3, [&] {
        ++executed;
        Action immediate(3, [&] { ++executed; });
        simulation.executeNow(immediate);
    }));
    simulation.schedule(std::make_unique<Action>(3, [&] { ++executed; }));
    pump.stepped = [&] { ++callbacks; pump.pause(); };
    pump.start(simulation);
    settle();
    check(callbacks == 1 && executed == 2 && order == std::vector<int>({0, 1}));
    pump.resume(); settle();
    check(callbacks == 2 && executed == 3 && order == std::vector<int>({0, 1, 2}));
    pump.stepped = [&] { ++callbacks; };
    pump.resume(); settle();
    check(callbacks == 3 && pump.state() == RuntimePump::State::Paused);
    settle(); check(callbacks == 3 && simulation.currentTime() == 3);
    bool failed = false;
    pump.failed = [&](const char*) { failed = true; };
    simulation.schedule(std::make_unique<Action>(4, [] { throw std::runtime_error("test"); }));
    pump.resume(); settle();
    check(failed && pump.state() == RuntimePump::State::Paused && simulation.currentTime() == 4);
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        pacingCalculations();
        delayedPump();
        completionAppearanceState();
        recurringPauseResume();
        atomicOrderingEmptyAndFailure();
        std::cout << "Runtime pump tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
