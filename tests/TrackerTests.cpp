#include <iostream>
#include <algorithm>
#include <set>
#include <memory>
#include <stdexcept>
#include "simulator/Tracker.hpp"
#include "simulator/TrackerAnnounceEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"
using namespace simulator;
namespace {
void check(bool value) { if (!value) throw std::runtime_error("Tracker assertion failed"); }
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
}
std::vector<Peer> peers(const std::vector<Swarm>& swarms, unsigned count = 4) {
    std::vector<Peer> result;
    for (unsigned id = 1; id <= count; ++id) {
        result.emplace_back(id, 1000000, 1000000, PeerProtocolId{static_cast<std::uint8_t>(id)});
        for (const auto& swarm : swarms) result.back().joinSwarm(swarm);
    }
    return result;
}
std::vector<Link> paths(unsigned count = 4) {
    std::vector<Link> result;
    for (unsigned a = 1; a <= count; ++a)
        for (unsigned b = a + 1; b <= count; ++b) result.emplace_back(a, b, 1000000, .01);
    return result;
}
std::set<PeerId> asSet(const std::vector<PeerId>& values) { return {values.begin(), values.end()}; }
void registry() {
    Tracker tracker(2);
    const InfoHash a{1}, b{2};
    check(tracker.announce(a, 3, 0).empty());
    check(tracker.announce(a, 1, 0).empty());
    check(tracker.announce(a, 2, 0).empty());
    check(asSet(tracker.announce(a, 1, 10)) == std::set<PeerId>({2, 3}));
    check(asSet(tracker.announce(a, 1, 10)) == std::set<PeerId>({2, 3}));
    check(tracker.announce(a, 4, 1).size() == 1);
    check(tracker.announce(a, 4, 100).size() == 2);
    check(tracker.announce(b, 1, 10).empty());
    check(tracker.announce(b, 5, 10) == std::vector<PeerId>({1}));
    const auto isolated = tracker.announce(a, 1, 10);
    check(isolated.size() == 2 && !asSet(isolated).contains(5) && !asSet(isolated).contains(1));
    Tracker disabled(0);
    check(disabled.announce(a, 1, 10).empty());
    check(disabled.announce(a, 2, 10).empty());
}
void seededSampling() {
    const InfoHash a{1}, b{2};
    Tracker first(7, 12345), repeat(7, 12345), different(7, 98765);
    for (PeerId id = 1; id <= 30; ++id) {
        for (auto* tracker : {&first, &repeat, &different}) {
            tracker->announce(a, id, 0);
            tracker->announce(b, id + 100, 0);
        }
    }
    bool variedSets = false;
    for (unsigned round = 0; round < 40; ++round) {
        const auto want = static_cast<std::size_t>(round % 11);
        for (const auto& hash : {a, b}) {
            const PeerId self = hash == a ? 1 : 101;
            const auto sample = first.announce(hash, self, want);
            check(sample == repeat.announce(hash, self, want));
            const auto other = different.announce(hash, self, want);
            variedSets |= asSet(sample) != asSet(other);
            check(sample.size() == std::min(want, std::size_t{7}));
            check(asSet(sample).size() == sample.size());
            for (const auto id : sample)
                check(id != self && id > self && id <= self + 29);
        }
    }
    check(variedSets);
    Tracker all(100, 42);
    for (PeerId id = 1; id <= 5; ++id) all.announce(a, id, 0);
    check(asSet(all.announce(a, 3, 100)) == std::set<PeerId>({1, 2, 4, 5}));
}
void simulationSeed() {
    auto scenario = [](std::uint64_t seed) {
        Swarm swarm(1, InfoHash{1}, 1);
        Simulation simulation(false, seed);
        check(simulation.seed() == seed);
        Network network(simulation, peers({swarm}, 10), paths(10), {swarm}, 3);
        for (PeerId id = 1; id < 10; ++id)
            simulation.schedule(std::make_unique<TrackerAnnounceEvent>(0, network, 1, id, 0));
        simulation.schedule(std::make_unique<TrackerAnnounceEvent>(0, network, 1, 10, 3));
        simulation.run();
        std::set<PeerId> result;
        for (const auto& [id, connection] : network.peer(10).swarmState(1).connections) {
            check(connection.handshakeComplete());
            result.insert(id);
        }
        check(result.size() == 3);
        return result;
    };
    const auto baseline = scenario(12345);
    check(baseline == scenario(12345));
    bool variation = false;
    for (std::uint64_t seed = 1; seed <= 5; ++seed) variation |= baseline != scenario(seed);
    check(variation);
}
void bounded() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation;
    Network network(simulation, peers({swarm}), paths(), {swarm}, 2);
    for (PeerId id = 1; id <= 3; ++id) network.announceToTracker(1, id, 0);
    network.announceToTracker(1, 4, 100);
    simulation.run();
    const auto& state = network.peer(4).swarmState(1);
    check(state.connections.size() == 2);

    for (const auto& [id, connection] : state.connections) {
        check(connection.handshakeComplete() && connection.bitfieldSent);
        check(connection.remoteBitfield == network.peer(id).swarmState(1).localBitfield);
    }
}
void reservations() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation;
    Network network(simulation, peers({swarm}), paths(), {swarm});
    for (PeerId id = 1; id <= 4; ++id) {
        network.setMaxNeighbors(1, id, 1);
    }
    network.announceToTracker(1, 1, 0);
    unsigned handshakes = 0;
    simulation.setEventObserver([&](const Event& event) {
        const auto* send = dynamic_cast<const SendMessageEvent*>(&event);
        if (send && send->details().messageType == MessageType::Handshake) ++handshakes;
    });
    network.announceToTracker(1, 2, 1); // Reserves 2--1 before any SendMessageEvent executes.
    check(network.peer(1).swarmState(1).connections.empty());
    check(network.peer(2).swarmState(1).connections.empty());
    rejects([&] { network.setMaxNeighbors(1, 1, 0); });
    rejects([&] { network.setMaxNeighbors(1, 2, 0); });
    network.announceToTracker(1, 1, 10); // Reverse attempt, while pending.
    network.announceToTracker(1, 2, 10); // Duplicate attempt, while pending.
    network.announceToTracker(1, 3, 10); // Both registered remotes are full; local 3 remains free.
    network.setMaxNeighbors(1, 3, 0);
    network.setMaxNeighbors(1, 3, 1);
    // All announce events precede their generated sends at the same timestamp.
    for (PeerId id : {3u, 4u, 3u, 4u})
        simulation.schedule(std::make_unique<TrackerAnnounceEvent>(0, network, 1, id, 10));
    simulation.run();
    check(handshakes == 4);
    for (PeerId id = 1; id <= 4; ++id) check(network.peer(id).swarmState(1).connections.size() == 1);
    check(network.peer(3).swarmState(1).connections.contains(4));
    for (PeerId id = 1; id <= 4; ++id) network.announceToTracker(1, id, 10);
    simulation.run();
    check(handshakes == 4);
    rejects([&] { network.setMaxNeighbors(1, 1, 0); });
}
void accumulated() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation;
    Network network(simulation, peers({swarm}), paths(), {swarm});
    network.setMaxNeighbors(1, 4, 2);
    network.announceToTracker(1, 1, 0);
    network.announceToTracker(1, 4, 1);
    simulation.run();
    check(network.peer(4).swarmState(1).connections.size() == 1);
    network.announceToTracker(1, 2, 0);
    network.announceToTracker(1, 3, 0);
    network.announceToTracker(1, 4, 10);
    rejects([&] { network.setMaxNeighbors(1, 4, 1); });
    simulation.run();
    const auto& connections = network.peer(4).swarmState(1).connections;
    check(connections.size() == 2 && connections.contains(1));
    network.announceToTracker(1, 3, 10); // Incoming attempt cannot exceed peer 4's cap.
    simulation.run();
    check(network.peer(4).swarmState(1).connections.size() == 2);
}
void missingPath() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation;
    Network network(simulation, peers({swarm}, 3), {Link(2, 3, 1000000, .01)}, {swarm});
    network.setMaxNeighbors(1, 3, 1);
    network.announceToTracker(1, 1, 0);
    network.announceToTracker(1, 2, 0);
    network.announceToTracker(1, 3, 10); // Only returned peer 2 has a path.
    simulation.run();
    check(network.peer(1).swarmState(1).connections.empty());
    check(network.peer(3).swarmState(1).connections.size() == 1);
    check(network.peer(3).swarmState(1).connections.contains(2));
    network.setMaxNeighbors(1, 1, 0); // Missing path never reserved the other endpoint.
}
void isolation() {
    Swarm a(1, InfoHash{1}, 1), b(2, InfoHash{2}, 1);
    Simulation simulation;
    Network network(simulation, peers({a, b}, 3), paths(3), {a, b});
    network.setMaxNeighbors(1, 1, 1);
    network.setMaxNeighbors(2, 1, 1);
    network.announceToTracker(1, 2, 0);
    network.announceToTracker(2, 3, 0);
    network.announceToTracker(1, 1, 10);
    network.announceToTracker(2, 1, 10);
    simulation.run();
    check(network.peer(1).swarmState(1).connections.size() == 1);
    check(network.peer(1).swarmState(1).connections.contains(2));
    check(network.peer(1).swarmState(2).connections.size() == 1);
    check(network.peer(1).swarmState(2).connections.contains(3));

    check(network.peer(3).swarmState(1).connections.empty());
    check(network.peer(2).swarmState(2).connections.empty());
}
void validation() {
    Swarm a(1, InfoHash{1}, 1), alias(2, InfoHash{1}, 1);
    Simulation simulation;
    Network ambiguous(simulation, peers({a, alias}, 2), paths(2), {a, alias});
    rejects([&] { ambiguous.announceToTracker(1, 1, 0); });
    rejects([&] { ambiguous.setMaxNeighbors(1, 1, 2); });
    Peer outsider(3, 1000000, 1000000);
    auto members = peers({a}, 2);
    members.push_back(outsider);
    Network network(simulation, members, paths(3), {a});
    rejects([&] { network.announceToTracker(1, 3, 0); });
    rejects([&] { network.announceToTracker(1, 99, 0); });
    rejects([&] { network.announceToTracker(99, 1, 0); });
    network.setMaxNeighbors(1, 1, 0);
    network.announceToTracker(1, 1, 0);
    network.announceToTracker(1, 2, 10);
    simulation.run();
    check(network.peer(2).swarmState(1).connections.empty());
}
void transfer() {
    Swarm swarm(1, InfoHash{1}, 20000, 16384);
    Peer seed(1, 1000000, 1000000, PeerProtocolId{1});
    Peer downloader(2, 1000000, 1000000, PeerProtocolId{2});
    seed.joinSwarm(swarm, {0xc0});
    downloader.joinSwarm(swarm);
    Simulation simulation;
    Network network(simulation, {seed, downloader}, paths(2), {swarm});
    simulation.schedule(std::make_unique<TrackerAnnounceEvent>(0, network, 1, 1, 10));
    simulation.schedule(std::make_unique<TrackerAnnounceEvent>(0, network, 1, 2, 10));
    simulation.run();
    const auto& state = network.peer(2).swarmState(1);
    check(state.localBitfield == seed.swarmState(1).localBitfield);
    check(state.connections.at(1).handshakeComplete() && state.connections.at(1).bitfieldSent);
    check(state.connections.at(1).remoteBitfield == state.localBitfield);
    check(state.connections.at(1).outgoingRequests.empty() && state.connections.at(1).scheduledRequests.empty());
    check(network.activeTransmissions().empty());
}
}
int main() {
    const std::pair<const char*, void(*)()> tests[] = {
        {"Seeded sampling reproducibility, variation, uniqueness, bounds and isolation", seededSampling},
        {"Simulation seed controls real tracker discovery", simulationSeed},
        {"Registry bounds, idempotence, self exclusion and hash isolation", registry},
        {"Deterministic bounded discovery and handshake/bitfield", bounded},
        {"Both endpoint caps, pending reservations, simultaneous/reverse and duplicate discovery", reservations},
        {"Accumulated established and pending neighbors", accumulated},
        {"Missing paths skip without reservations", missingPath},
        {"Per-peer/swarm discovery isolation", isolation},
        {"Membership, ambiguous hashes and zero capacity", validation},
        {"Tracker startup completes a small transfer", transfer}
    };
    unsigned failed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "Tests: " << std::size(tests) << ", failed: " << failed << '\n';
    return failed ? 1 : 0;
}
