#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <set>
#include "simulator/Network.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/RechokeEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/TransmissionStartEvent.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
using namespace simulator;
namespace {
void check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Choking assertion at line " + std::to_string(where.line()));
}
class Action : public Event {
public:
    Action(double t, std::function<void()> f) : Event(t), f_(std::move(f)) {}
    void execute() override { f_(); }
private: std::function<void()> f_;
};
struct Fixture {
    Swarm a{1, InfoHash{1}, 1048576, 1048576}, b{2, InfoHash{2}, 1048576, 1048576};
    Simulation simulation;
    Network network;
    std::vector<Peer> peers(bool seed) {
        std::vector<Peer> result;
        for (PeerId id = 1; id <= 7; ++id) {
            result.emplace_back(id, 1e7, 1e7);
            result.back().joinSwarm(a, {static_cast<std::uint8_t>(id == 1 ? (seed ? 0x80 : 0) : (seed ? 0 : 0x80))});
            result.back().joinSwarm(b, {static_cast<std::uint8_t>(id == 1 ? 0x80 : 0)});
        }
        for (const auto* swarm : {&a, &b}) for (PeerId id = 2; id <= 7; ++id) {
            auto& local = result[0]; auto& remote = result[id - 1];
            local.markHandshakeSent(*swarm, id); remote.markHandshakeSent(*swarm, 1);
            local.receiveMessage(*swarm, id, Message(MessageType::Handshake, HandshakePayload{swarm->infoHash(), {}}), &remote.protocolId());
            remote.receiveMessage(*swarm, 1, Message(MessageType::Handshake, HandshakePayload{swarm->infoHash(), {}}), &local.protocolId());
        }
        return result;
    }
    explicit Fixture(bool seed = false) : network(simulation, peers(seed),
        {Link(1, 2, 1e7, .01), Link(1, 3, 1e7, .01), Link(1, 4, 1e7, .01),
         Link(1, 5, 1e7, .01), Link(1, 6, 1e7, .01), Link(1, 7, 1e7, .01)}, {a, b}) {}
    PeerSwarmState& state(PeerId id = 1, SwarmId swarm = 1) {
        return const_cast<PeerSwarmState&>(network.peer(id).swarmState(swarm));
    }
    void at(double t, std::function<void()> f) { simulation.schedule(std::make_unique<Action>(t, std::move(f))); }
    void interest(SwarmId swarm = 1) {
        for (PeerId id = 2; id <= 7; ++id) network.deliver(swarm, id, 1, Message(MessageType::Interested));
    }
    void tick(double t, SwarmId swarm = 1) { simulation.schedule(std::make_unique<RechokeEvent>(t, network, 1, swarm)); }
    void stop(double t) {
        at(t, [&] {
            for (SwarmId swarm : {1u, 2u}) for (PeerId id = 2; id <= 7; ++id) {
                network.deliver(swarm, id, 1, Message(MessageType::NotInterested));
                network.deliver(swarm, 1, id, Message(MessageType::NotInterested));
                state(id, swarm).connections.at(1).weAreInterestedInRemote = false;
                state(1, swarm).connections.at(id).weAreInterestedInRemote = false;
            }
        });
    }
    std::set<PeerId> selected(SwarmId swarm = 1) {
        std::set<PeerId> ids;
        for (const auto& [id, c] : state(1, swarm).connections) if (c.remoteInterestedInUs && !c.weAreChokingRemote) ids.insert(id);
        return ids;
    }
    void payload(PeerId from, PeerId to, std::uint32_t begin, std::uint32_t bytes, SwarmId swarm = 1) {
        const RequestPayload request{0, begin, bytes};
        state(from, swarm).connections.at(to).weAreChokingRemote = false;
        state(from, swarm).connections.at(to).acceptedRequests.push_back(request);
        state(to, swarm).connections.at(from).outgoingRequests.push_back(request);
        network.send(swarm, from, to, Message(MessageType::Piece, PiecePayload{0, begin, bytes}));
    }
};
void initial() {
    Fixture f;
    f.interest();
    check(f.selected().empty());
    f.at(9.99, [&] { check(f.selected().empty()); });
    f.simulation.run();
    check(f.selected().size() == 4);
}
void cap() {
    Fixture f; f.interest();
    f.at(10.001, [&] { check(f.selected() == std::set<PeerId>({2,3,4,6})); });
    f.simulation.run();
}
void leecher() {
    Fixture f; f.interest();
    for (PeerId id = 2; id <= 7; ++id) f.payload(id, 1, (id - 2) * 1000, (id - 1) * 100);
    f.at(10.001, [&] {
        check(f.selected() == std::set<PeerId>({2,5,6,7}));
        for (PeerId id = 2; id <= 7; ++id) {
            const auto& c = f.state().connections.at(id);
            check(c.recentDownloadRate == (id - 1) * 10 && c.downloadedInWindow == 0);
        }
    });
    f.simulation.run();
}
void seeder() {
    Fixture f(true); f.interest();
    for (PeerId id = 2; id <= 7; ++id) {
        f.payload(1, id, 0, (8 - id) * 100);
        f.state().connections.at(id).downloadedInWindow = id * 10000; // Conflicting metric must be ignored.
    }
    f.at(10.001, [&] {
        check(f.selected() == std::set<PeerId>({2,3,4,6}));
        check(f.state().connections.at(2).recentUploadRate == 60);
    });
    f.stop(11); f.simulation.run();
}
void timing() {
    Fixture f; f.interest();
    f.at(10.001, [&] { check(f.selected() == std::set<PeerId>({2,3,4,6})); });
    f.at(11, [&] { f.payload(7, 1, 0, 1000); });
    f.at(19.99, [&] { check(f.selected() == std::set<PeerId>({2,3,4,6})); });
    f.tick(20);
    f.at(20.001, [&] { check(f.selected() == std::set<PeerId>({2,3,6,7})); });
    f.simulation.run();
}
void optimistic() {
    Fixture f; f.interest();
    f.at(10.001, [&] { check(f.state().choking.optimistic == 6 && f.selected().contains(6)); });
    f.simulation.run();
}
void optimisticSlot() {
    Fixture f; f.interest();
    f.at(10.001, [&] { check(f.selected().size() == 4 && !f.selected().contains(5)); });
    f.simulation.run();
}
void rotation() {
    Fixture f; f.interest(); f.tick(20); f.tick(30); f.tick(60);
    f.at(20.001, [&] { check(f.state().choking.optimistic == 6); });
    f.at(30.001, [&] { check(f.state().choking.optimistic == 7 && f.selected().contains(7)); });
    f.at(60.001, [&] { check(f.state().choking.optimistic == 6); });
    f.simulation.run();
}
void notInterested() {
    Fixture f; f.interest(); f.tick(20);
    f.at(11, [&] {
        f.network.deliver(1, 2, 1, Message(MessageType::NotInterested));
        check(f.selected().size() == 3 && !f.state().connections.at(2).weAreChokingRemote);
    });
    f.at(20.001, [&] { check(f.selected().size() == 4 && f.state().connections.at(2).weAreChokingRemote); });
    f.simulation.run();
}
void suppression() {
    Fixture f; f.interest(); f.tick(20);
    unsigned unchokes = 0;
    f.simulation.setEventObserver([&](const Event& event) {
        if (const auto* send = dynamic_cast<const SendMessageEvent*>(&event);
            send && send->details().sender == 1 && send->details().messageType == MessageType::Unchoke) ++unchokes;
    });
    f.simulation.run(); check(unchokes == 4);
}
void isolation() {
    Fixture f; f.interest(); f.interest(2);
    for (PeerId id = 2; id <= 7; ++id) {
        f.payload(id, 1, (id - 2) * 1000, (id - 1) * 100);
        f.payload(1, id, 0, (8 - id) * 100, 2);
    }
    f.at(10.001, [&] {
        check(f.selected(1) == std::set<PeerId>({2,5,6,7}));
        check(f.selected(2) == std::set<PeerId>({2,3,4,6}));
        check(f.state(1,1).connections.at(7).recentUploadRate == 0);
        check(f.state(1,2).connections.at(7).recentDownloadRate == 0);
    });
    f.stop(11); f.simulation.run();
}
void determinism() {
    std::vector<std::set<PeerId>> expected;
    for (int run = 0; run < 2; ++run) {
        Fixture f; f.interest(); f.tick(30); f.tick(60);
        std::vector<std::set<PeerId>> actual;
        for (double t : {10.001,30.001,60.001}) f.at(t, [&] { actual.push_back(f.selected()); });
        f.simulation.run();
        if (run == 0) expected = actual; else check(actual == expected);
    }
}
void usefulOnly() {
    Fixture f;
    f.payload(2, 1, 0, 100);
    f.at(1, [&] {
        check(f.state().connections.at(2).downloadedInWindow == 100);
        f.payload(3, 1, 50, 100); // Only 50 new useful bytes.
        f.network.send(1, 2, 1, Message(MessageType::Cancel, CancelPayload{}));
    });
    f.at(2, [&] {
        check(f.state().connections.at(3).downloadedInWindow == 50);
        check(f.state(3).connections.at(1).uploadedInWindow == 50);
        check(f.state().connections.at(2).downloadedInWindow == 100);
    });
    f.simulation.run();
}
void rollingRates(bool seed) {
    Fixture f(seed);
    auto transfer = [&](std::uint32_t begin, std::uint32_t bytes) {
        f.payload(seed ? 1 : 2, seed ? 2 : 1, begin, bytes);
    };
    auto rate = [&] {
        const auto& c = f.state().connections.at(2);
        return seed ? c.recentUploadRate : c.recentDownloadRate;
    };
    f.at(1, [&] { transfer(0, 1000); });
    f.tick(10);
    f.at(10.001, [&] {
        check(rate() == 100); // Warm-up: 1000 / 10, not / 20.
        const auto& c = f.state().connections.at(2);
        check((seed ? c.uploadedPreviousInterval : c.downloadedPreviousInterval) == 1000);
        check(c.downloadedInWindow == 0 && c.uploadedInWindow == 0);
        check((seed ? c.recentDownloadRate : c.recentUploadRate) == 0);
    });
    f.at(11, [&] { transfer(1000, 200); });
    f.tick(20);
    f.at(20.001, [&] { check(rate() == 60); }); // (1000 + 200) / 20.
    f.at(21, [&] { transfer(1200, 600); });
    f.tick(30);
    f.at(30.001, [&] { check(rate() == 40); }); // (200 + 600) / 20; first 1000 expired.
    f.simulation.run();
}
void rollingBurstRanking() {
    Fixture f; f.interest();
    // Sustained earlier contributors must outrank a small latest-interval burst.
    for (PeerId id = 2; id <= 5; ++id) f.payload(id, 1, (id - 2) * 10000, (11 - id) * 1000);
    f.at(10.001, [&] { check(f.selected() == std::set<PeerId>({2,3,4,6})); });
    f.at(19, [&] { f.payload(7, 1, 50000, 1000); });
    f.tick(20);
    f.at(20.001, [&] {
        check(f.state().connections.at(7).recentDownloadRate == 50); // Not 100 B/s.
        check(f.state().connections.at(2).recentDownloadRate == 450);
        check(f.selected() == std::set<PeerId>({2,3,4,6})); // A tumbling window would promote peer 7.
    });
    f.simulation.run();
}
void rollingIdleRestart() {
    for (const double resume : {15.0, 25.0, 35.0}) {
        Fixture f;
        f.payload(2, 1, 0, 1000);
        f.tick(10);
        f.at(10.001, [&] { check(!f.state().choking.eventPending); });
        f.at(11, [&] { f.payload(2, 1, 1000, 200); });
        f.at(resume, [&] { f.interest(); }); // Restarts the ordinary policy timer.
        const double decision = resume + 5;
        f.at(decision + .001, [&, resume] {
            const double expected = resume == 15 ? 60 : resume == 25 ? 10 : 0;
            check(f.state().connections.at(2).recentDownloadRate == expected);
            check(!f.state().choking.eventPending);
        });
        f.simulation.run();
        check(f.simulation.currentTime() < decision + 1); // No perpetual policy timer.
    }
}
void rollingSwarmIsolation() {
    Fixture f;
    f.payload(2, 1, 0, 1000, 1);
    f.payload(1, 2, 0, 2000, 2);
    f.tick(10, 1); f.tick(10, 2);
    f.at(11, [&] {
        f.payload(2, 1, 1000, 200, 1);
        f.payload(1, 2, 2000, 600, 2);
    });
    f.tick(20, 1); f.tick(20, 2);
    f.at(20.001, [&] {
        const auto& leecher = f.state(1, 1).connections.at(2);
        const auto& seeder = f.state(1, 2).connections.at(2);
        check(leecher.recentDownloadRate == 60 && leecher.recentUploadRate == 0);
        check(seeder.recentUploadRate == 130 && seeder.recentDownloadRate == 0);
    });
    f.simulation.run();
}
void realPipeline(bool many) {
    const Swarm swarm(1, InfoHash{7}, 65536, 65536);
    const PeerId count = many ? 7 : 2;
    std::vector<Peer> peers; std::vector<Link> links;
    for (PeerId id = 1; id <= count; ++id) {
        peers.emplace_back(id, many ? 65536 : 1e6, many ? 65536 : 1e6);
        peers.back().joinSwarm(swarm, {static_cast<std::uint8_t>(id == 1 ? 0x80 : 0)});
        if (id != 1) links.emplace_back(1, id, 1e6, .01);
    }
    Simulation simulation; Network network(simulation, peers, links, {swarm});
    unsigned requests = 0, pieces = 0, decisions = 0;
    bool four = false, rotated = false;
    std::optional<PeerId> initialOptimistic;
    simulation.setEventObserver([&](const Event& event) {
        check(event.time() < 500);
        const auto& state = network.peer(1).swarmState(1);
        unsigned selected = 0;
        for (const auto& [id, c] : state.connections) if (c.remoteInterestedInUs && !c.weAreChokingRemote) ++selected;
        check(selected <= 4); four |= selected == 4;
        if (state.choking.optimistic) {
            if (!initialOptimistic) initialOptimistic = state.choking.optimistic;
            else rotated |= initialOptimistic != state.choking.optimistic;
        }
        if (const auto* tick = dynamic_cast<const RechokeEvent*>(&event); tick && tick->peerId() == 1) ++decisions;
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event)) {
            if (start->message().type() == MessageType::Request) { ++requests; check(event.time() >= 10); }
            if (start->message().type() == MessageType::Piece) ++pieces;
        }
    });
    for (PeerId id = 2; id <= count; ++id) network.send(1, id, 1,
        Message(MessageType::Handshake, HandshakePayload{swarm.infoHash(), peers[id - 1].protocolId()}));
    simulation.run();
    for (PeerId id = 1; id <= count; ++id) check(network.peer(id).swarmState(1).localBitfield[0] == 0x80);
    check(requests >= (count - 1) * 4 && pieces >= (count - 1) * 4);
    if (many) check(four && rotated && decisions >= 3);
}
}
int main() {
    std::cout << std::unitbuf;
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"Initial choked state", initial}, {"Four interested slots", cap},
        {"Leecher delivered download ranking", leecher}, {"Seeder delivered upload ranking", seeder},
        {"Ten-second decisions and rolling rate", timing}, {"Nonpreferred optimistic peer", optimistic},
        {"Optimistic counts as slot", optimisticSlot}, {"Thirty-second rotation", rotation},
        {"Not interested frees slot without immediate churn", notInterested},
        {"Unchanged transition suppression", suppression}, {"Swarm and seeder-status isolation", isolation},
        {"Deterministic ties and rotation", determinism}, {"Useful payload only, no overlap/control credit", usefulOnly},
        {"Leecher rolling window: warm-up, accumulation, expiration", [] { rollingRates(false); }},
        {"Seeder rolling window: warm-up, accumulation, expiration", [] { rollingRates(true); }},
        {"Latest-interval burst retains historical ranking", rollingBurstRanking},
        {"Rolling buckets survive idle restart and expire", rollingIdleRestart},
        {"Rolling upload/download measurements remain swarm-isolated", rollingSwarmIsolation},
        {"Unchoke resumes real request pipeline", [] { realPipeline(false); }},
        {"Seven-peer periodic choking integration", [] { realPipeline(true); }}
    };
    unsigned failed = 0;
    for (const auto& test : tests) {
        try { test.run(); std::cout << "PASS: " << test.name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL: " << test.name << ": " << e.what() << '\n'; }
    }
    std::cout << "Tests: " << std::size(tests) << ", failed: " << failed << '\n';
    return failed ? 1 : 0;
}