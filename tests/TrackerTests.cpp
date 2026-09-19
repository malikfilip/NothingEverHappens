#include <iostream>
#include <cmath>
#include <limits>
#include <source_location>
#include "simulator/MessageArrivalEvent.hpp"
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
void check(bool value, const std::source_location where = std::source_location::current()) { if (!value) throw std::runtime_error("Tracker assertion failed at line " + std::to_string(where.line())); }
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
    Simulation simulation(false, 7);
    Network network(simulation, peers({swarm}, 2), paths(2), {swarm});
    for (PeerId id : {1u, 2u}) network.setTargetOutgoingConnections(1, id, 1);
    unsigned handshakes = 0;
    simulation.setEventObserver([&](const Event& event) {
        if (const auto* send = dynamic_cast<const SendMessageEvent*>(&event);
            send && send->details().messageType == MessageType::Handshake) ++handshakes;
    });
    network.announceToTracker(1, 2, 0);
    network.announceToTracker(1, 1, 1); // A owns the pending A -> B attempt.
    rejects([&] { network.setTargetOutgoingConnections(1, 1, 0); });
    network.announceToTracker(1, 2, 1); // Reverse discovery while pending.
    network.announceToTracker(1, 1, 1); // Duplicate discovery while pending.
    network.setTargetOutgoingConnections(1, 2, 0); // Incoming did not acquire ownership.
    network.setTargetOutgoingConnections(1, 2, 1);
    simulation.run();
    check(handshakes == 2);
    for (PeerId id : {1u, 2u}) {
        const auto& connections = network.peer(id).swarmState(1).connections;
        check(connections.size() == 1 && connections.at(3 - id).handshakeComplete());
    }
    network.announceToTracker(1, 2, 1); // Reverse discovery after establishment.
    network.announceToTracker(1, 1, 1);
    simulation.run();
    check(handshakes == 2);
    rejects([&] { network.setTargetOutgoingConnections(1, 1, 0); });
    network.setTargetOutgoingConnections(1, 2, 0);
}
void accumulated() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation(false, 7);
    // A=1, B=2, C=3, D=4. Only B--A is a usable path for B.
    Network network(simulation, peers({swarm}),
        {Link(1, 2, 1000000, .01), Link(1, 3, 1000000, .01), Link(1, 4, 1000000, .01)}, {swarm});
    network.setTargetOutgoingConnections(1, 1, 2);
    network.setTargetOutgoingConnections(1, 2, 1);
    network.announceToTracker(1, 3, 0);
    network.announceToTracker(1, 4, 0);
    network.announceToTracker(1, 1, 2);
    rejects([&] { network.setTargetOutgoingConnections(1, 1, 1); });
    network.announceToTracker(1, 2, 0);
    network.announceToTracker(1, 1, 3); // Pending ownership prevents oversubscription toward B.
    simulation.run();
    const auto& a = network.peer(1).swarmState(1).connections;
    check(a.size() == 2 && a.contains(3) && a.contains(4) && !a.contains(2));
    // A response of three includes A and two unusable candidates; admission remains sparse.
    network.announceToTracker(1, 2, 3);
    rejects([&] { network.setTargetOutgoingConnections(1, 2, 0); });
    simulation.run();
    check(a.size() == 3);
    for (PeerId remote : {2u, 3u, 4u}) check(a.at(remote).handshakeComplete());
    check(network.peer(2).swarmState(1).connections.size() == 1
        && network.peer(2).swarmState(1).connections.at(1).handshakeComplete());
    network.setTargetOutgoingConnections(1, 1, 2); // Exactly two owned, despite three neighbors.
    rejects([&] { network.setTargetOutgoingConnections(1, 1, 1); });
    network.setTargetOutgoingConnections(1, 2, 1);
    rejects([&] { network.setTargetOutgoingConnections(1, 2, 0); });
    network.setTargetOutgoingConnections(1, 3, 0);
    network.setTargetOutgoingConnections(1, 4, 0);
}
void manualHandshakeAccounting() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation;
    Network network(simulation, peers({swarm}, 3), paths(3), {swarm});
    network.setTargetOutgoingConnections(1, 1, 0);
    simulation.schedule(std::make_unique<SendMessageEvent>(0, network, 1, 1, 2,
        Message(MessageType::Handshake, HandshakePayload{swarm.infoHash(), network.peer(1).protocolId()})));
    simulation.run();
    check(network.peer(1).swarmState(1).connections.at(2).handshakeComplete());
    network.setTargetOutgoingConnections(1, 1, 0);
    network.setTargetOutgoingConnections(1, 2, 0);
    network.setTargetOutgoingConnections(1, 1, 1);
    network.announceToTracker(1, 3, 0);
    network.announceToTracker(1, 1, 1);
    simulation.run();
    check(network.peer(1).swarmState(1).connections.size() == 2
        && network.peer(1).swarmState(1).connections.at(3).handshakeComplete());
    rejects([&] { network.setTargetOutgoingConnections(1, 1, 0); });
}
void missingPath() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation;
    Network network(simulation, peers({swarm}, 3), {Link(2, 3, 1000000, .01)}, {swarm});
    network.setTargetOutgoingConnections(1, 3, 1);
    network.announceToTracker(1, 1, 0);
    network.announceToTracker(1, 2, 0);
    network.announceToTracker(1, 3, 10); // Only returned peer 2 has a path.
    simulation.run();
    check(network.peer(1).swarmState(1).connections.empty());
    check(network.peer(3).swarmState(1).connections.size() == 1);
    check(network.peer(3).swarmState(1).connections.contains(2));
    network.setTargetOutgoingConnections(1, 1, 0); // Missing path never reserved the other endpoint.
}
void isolation() {
    Swarm a(1, InfoHash{1}, 1), b(2, InfoHash{2}, 1);
    Simulation simulation;
    Network network(simulation, peers({a, b}, 3), paths(3), {a, b});
    network.setTargetOutgoingConnections(1, 1, 1);
    network.setTargetOutgoingConnections(2, 1, 1);
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
    rejects([&] { ambiguous.setTargetOutgoingConnections(1, 1, 2); });
    Peer outsider(3, 1000000, 1000000);
    auto members = peers({a}, 2);
    members.push_back(outsider);
    Network network(simulation, members, paths(3), {a});
    rejects([&] { network.announceToTracker(1, 3, 0); });
    rejects([&] { network.announceToTracker(1, 99, 0); });
    rejects([&] { network.announceToTracker(99, 1, 0); });
    network.setTargetOutgoingConnections(1, 1, 0);
    network.announceToTracker(1, 2, 0);
    network.announceToTracker(1, 1, 1); // Zero target does not initiate despite a usable candidate.
    simulation.run();
    check(network.peer(1).swarmState(1).connections.empty());
    network.announceToTracker(1, 2, 1);
    simulation.run();
    check(network.peer(1).swarmState(1).connections.at(2).handshakeComplete());
    check(network.peer(2).swarmState(1).connections.at(1).handshakeComplete());
    network.setTargetOutgoingConnections(1, 1, 0);
    rejects([&] { network.setTargetOutgoingConnections(1, 2, 0); });
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
void lazyCreationAndReuse() {
    // An existing reverse-oriented path and a lazily created path must behave identically.
    for (const bool preconfigured : {false, true}) {
        Swarm a(1, InfoHash{1}, 1), b(2, InfoHash{2}, 1);
        Simulation simulation(false, 7);
        std::vector<Link> links;
        if (preconfigured) links.emplace_back(2, 1, 4000, .25);
        Network network(simulation, peers({a, b}, 3), std::move(links), {a, b});
        unsigned calls = 0;
        network.setLinkConfigProvider([&](PeerId from, PeerId to) {
            ++calls;
            check(from == 1 && (to == 2 || to == 3));
            return LinkConfig{4000, .25};
        });
        unsigned firstArrivals = 0;
        simulation.setEventObserver([&](const Event& event) {
            const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event);
            if (!arrival) return;
            const auto details = arrival->details();
            if (details.swarmId == 1 && details.sender == 1 && details.receiver == 2
                && details.messageType == MessageType::Handshake) {
                ++firstArrivals;
                const Message message(MessageType::Handshake, HandshakePayload{a.infoHash(), network.peer(1).protocolId()});
                check(std::abs(event.time() - (message.wireSize() * 8.0 / 4000 + .25)) < 1e-9);
            }
        });
        network.announceToTracker(1, 2, 0);
        network.announceToTracker(1, 1, 1);
        check(calls == (preconfigured ? 0u : 1u));
        network.announceToTracker(1, 2, 1); // Reverse pending attempt.
        network.announceToTracker(1, 1, 1); // Duplicate pending attempt.
        check(simulation.step()); // Start the first handshake on link index zero.
        check(network.activeTransmissions().size() == 1);
        check(network.activeTransmissions().begin()->second.linkIndex == 0);
        check(network.activeTransmissions().begin()->second.currentRate == 4000);

        // Append while an old link is active; its completion must retain the correct index.
        network.announceToTracker(1, 3, 0);
        network.announceToTracker(1, 1, 3);
        check(simulation.step());
        check(network.activeTransmissions().size() == 2);
        std::set<std::size_t> indices;
        for (const auto& [id, active] : network.activeTransmissions()) indices.insert(active.linkIndex);
        check(indices == std::set<std::size_t>({0, 1})); // No hidden duplicate was appended.

        // Another swarm initiates in the reverse direction on the same physical pair.
        network.announceToTracker(2, 1, 0);
        network.announceToTracker(2, 2, 1);
        simulation.run();
        check(firstArrivals == 1 && network.activeTransmissions().empty());
        check(network.peer(1).swarmState(1).connections.at(2).handshakeComplete());
        check(network.peer(1).swarmState(1).connections.at(3).handshakeComplete());
        check(network.peer(2).swarmState(2).connections.at(1).handshakeComplete());
        check(network.peer(1).swarmState(2).connections.at(2).handshakeComplete());
        network.announceToTracker(1, 1, 3);
        network.announceToTracker(2, 2, 1);
        check(!simulation.step());
        check(calls == (preconfigured ? 1u : 2u));
    }
}

void noProviderAndInvalidConfiguration() {
    const Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation;
    Network network(simulation, peers({swarm}, 2), {}, {swarm});
    network.announceToTracker(1, 2, 0);
    network.announceToTracker(1, 1, 1);
    check(!simulation.step());
    rejects([&] { network.transmissionState(1, 2); });
    check(network.peer(1).swarmState(1).connections.empty());
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto config : {LinkConfig{0, 0}, LinkConfig{-1, 0}, LinkConfig{infinity, 0},
                             LinkConfig{nan, 0}, LinkConfig{1000, -1},
                             LinkConfig{1000, infinity}, LinkConfig{1000, nan}}) {
        unsigned calls = 0;
        network.setLinkConfigProvider([&](PeerId, PeerId) { ++calls; return config; });
        network.announceToTracker(1, 1, 1);
        check(calls == 1 && !simulation.step());
        rejects([&] { network.transmissionState(1, 2); });
        network.setTargetOutgoingConnections(1, 1, 0); // Rejected attempts reserve nothing.
        network.setTargetOutgoingConnections(1, 1, 1);
    }
    network.setLinkConfigProvider([](PeerId, PeerId) -> LinkConfig { throw std::invalid_argument("provider"); });
    rejects([&] { network.announceToTracker(1, 1, 1); });
    rejects([&] { network.transmissionState(1, 2); });
    network.setLinkConfigProvider({});
    network.announceToTracker(1, 1, 1);
    check(!simulation.step());
    network.setLinkConfigProvider([](PeerId, PeerId) { return LinkConfig{1000, 0}; });
    network.announceToTracker(1, 1, 1);
    check(simulation.step());
    check(network.activeTransmissions().begin()->second.linkIndex == 0);
    simulation.run();
    check(network.peer(1).swarmState(1).connections.at(2).handshakeComplete());
}

void lazyOutgoingLimit() {
    const Swarm swarm(1, InfoHash{1}, 1);
    Simulation simulation(false, 42);
    Network network(simulation, peers({swarm}, 6), {}, {swarm});
    std::set<PeerId> admitted;
    network.setLinkConfigProvider([&](PeerId from, PeerId to) {
        check(from == 1 && admitted.insert(to).second);
        return LinkConfig{1000000, .01};
    });
    for (PeerId id = 2; id <= 6; ++id) network.announceToTracker(1, id, 0);
    network.setTargetOutgoingConnections(1, 1, 0);
    network.announceToTracker(1, 1, 20);
    check(admitted.empty() && !simulation.step());
    network.setTargetOutgoingConnections(1, 1, 2);
    network.announceToTracker(1, 1, 20);
    network.announceToTracker(1, 1, 20); // Pending attempts also consume the target.
    check(admitted.size() == 2);
    simulation.run();
    check(network.peer(1).swarmState(1).connections.size() == 2);
    network.announceToTracker(1, 1, 20); // Established attempts still consume it.
    check(!simulation.step() && admitted.size() == 2);
    for (PeerId a = 1; a <= 6; ++a) {
        for (PeerId b = a + 1; b <= 6; ++b) {
            if (a == 1 && admitted.contains(b)) {
                check(network.peer(1).swarmState(1).connections.at(b).handshakeComplete());
                check(!network.transmissionState(a, b).active);
            } else {
                rejects([&] { network.transmissionState(a, b); });
            }
        }
    }
}

void lazyRejectsUnusablePeers() {
    const Swarm swarm(1, InfoHash{1}, 1);
    for (const bool zeroUpload : {false, true}) {
        Peer local(1, zeroUpload ? 0 : 1000, zeroUpload ? 1000 : 0);
        local.joinSwarm(swarm);
        auto members = peers({swarm}, 2);
        members[0] = local;
        Simulation simulation;
        Network network(simulation, std::move(members), {}, {swarm});
        unsigned calls = 0;
        network.setLinkConfigProvider([&](PeerId, PeerId) { ++calls; return LinkConfig{1000, 0}; });
        network.announceToTracker(1, 2, 0);
        network.announceToTracker(1, 1, 1);
        check(calls == 0 && !simulation.step());
        rejects([&] { network.transmissionState(1, 2); });
    }
}
}
int main() {
    const std::pair<const char*, void(*)()> tests[] = {
        {"Lazy and preconfigured paths reuse physical indices across orientations and swarms", lazyCreationAndReuse},
        {"No provider and invalid configuration leave no path or reservation", noProviderAndInvalidConfiguration},
        {"Outgoing targets prevent unnecessary lazy paths, pending and established", lazyOutgoingLimit},
        {"Unusable peer capacities do not create paths", lazyRejectsUnusablePeers},
        {"Seeded sampling reproducibility, variation, uniqueness, bounds and isolation", seededSampling},
        {"Simulation seed controls real tracker discovery", simulationSeed},
        {"Registry bounds, idempotence, self exclusion and hash isolation", registry},
        {"Deterministic bounded discovery and handshake/bitfield", bounded},
        {"Outgoing ownership survives pending and established reverse/duplicate discovery", reservations},
        {"Full outgoing target accepts incoming without acquiring ownership", accumulated},
        {"Manual handshakes do not consume tracker outgoing capacity", manualHandshakeAccounting},
        {"Missing paths skip without reservations", missingPath},
        {"Per-peer/swarm discovery isolation", isolation},
        {"Membership, ambiguous hashes and zero outgoing target accepts incoming", validation},
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
