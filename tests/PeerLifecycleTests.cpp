#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include "simulator/PeerJoinEvent.hpp"
#include "simulator/PeerLeaveEvent.hpp"
#include "simulator/TrackerAnnounceEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/TransmissionStartEvent.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
#include "simulator/RechokeEvent.hpp"
#include "simulator/Simulation.hpp"
using namespace simulator;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Expected invalid_argument");
}
class Action : public Event {
public:
    Action(double time, std::function<void()> action) : Event(time), action_(std::move(action)) {}
    void execute() override { action_(); }
private:
    std::function<void()> action_;
};
void at(Simulation& sim, double time, std::function<void()> action) {
    sim.schedule(std::make_unique<Action>(time, std::move(action)));
}
std::vector<Peer> makePeers(unsigned count, double rate = 1000000) {
    std::vector<Peer> peers;
    for (PeerId id = 1; id <= count; ++id) peers.emplace_back(id, rate, rate, PeerProtocolId{static_cast<std::uint8_t>(id)});
    return peers;
}
std::vector<Link> paths(unsigned count, double rate = 1000000, double latency = .01) {
    std::vector<Link> links;
    for (PeerId a = 1; a <= count; ++a) for (PeerId b = a + 1; b <= count; ++b) links.emplace_back(a, b, rate, latency);
    return links;
}
void handshake(Simulation& sim, Network& net, const Swarm& swarm, PeerId a, PeerId b) {
    sim.schedule(std::make_unique<SendMessageEvent>(sim.currentTime(), net, swarm.id(), a, b,
        Message(MessageType::Handshake, HandshakePayload{swarm.infoHash(), net.peer(a).protocolId()})));
}
void leaveAt(Simulation& sim, Network& net, double time, PeerId peer, SwarmId swarm = 1) {
    sim.schedule(std::make_unique<PeerLeaveEvent>(time, net, swarm, peer));
}
void drained(const Network& net, unsigned count) {
    check(net.activeTransmissions().empty(), "Active transmission leaked");
    for (PeerId a = 1; a <= count; ++a) for (PeerId b = 1; b <= count; ++b) if (a != b) {
        const auto state = net.transmissionState(a, b);
        check(!state.active && state.pendingCount == 0, "FIFO leaked");
    }
}
void independentPeriodicNumwant() {
    const Swarm swarm(1, InfoHash{1}, 1), other(2, InfoHash{2}, 1);
    auto members = makePeers(6);
    for (auto& peer : members) {
        if (peer.id() != 1) peer.joinSwarm(swarm);
        peer.joinSwarm(other);
    }
    Simulation sim(false, 12345);
    Network net(sim, std::move(members), {}, {swarm, other}, 50, 2);
    Tracker expected(50, 12345, 2);
    for (PeerId id = 2; id <= 6; ++id) {
        net.announceToTracker(1, id, 0);
        expected.announce(swarm.infoHash(), id, 0);
    }
    for (PeerId id = 1; id <= 5; ++id) {
        net.announceToTracker(2, id, 0);
        expected.announce(other.infoHash(), id, 0);
    }
    const auto started = expected.announce(swarm.infoHash(), 1, 3, AnnounceKind::Started);
    const auto regular = expected.announce(swarm.infoHash(), 1, 3);
    expected.announce(swarm.infoHash(), 1, 3); // At capacity, still sample all three.
    const auto probe = expected.announce(other.infoHash(), 6, 1);
    std::vector<PeerId> startedCalls, regularCalls;
    unsigned probeCalls = 0;
    net.setLinkConfigProvider([&](PeerId from, PeerId to) {
        if (sim.currentTime() == 0) {
            check(from == 1, "Unexpected STARTED initiator");
            startedCalls.push_back(to);
            return LinkConfig{0, 0}; // See every candidate without using any slots.
        }
        if (sim.currentTime() == 2) {
            check(from == 1, "Unexpected regular initiator");
            regularCalls.push_back(to);
            return LinkConfig{regularCalls.size() == 1 ? 0.0 : 1000000.0, .01};
        }
        check(near(sim.currentTime(), 4.5) && from == 6 && to == probe.front(),
            "Full-target announce changed configured numwant/RNG sampling");
        ++probeCalls;
        return LinkConfig{1000000, .01};
    });
    net.joinSwarm(1, 1, JoinOptions{3, 1, std::nullopt});
    at(sim, .5, [&] {
        check(startedCalls == started, "STARTED numwant was clamped to outgoing slots");
        check(net.peer(1).swarmState(1).connections.empty(), "Invalid configuration admitted a connection");
    });
    at(sim, 2.5, [&] {
        check(regularCalls == std::vector<PeerId>({regular[0], regular[1]}),
            "Regular announce did not try beyond the first rejected candidate");
        const auto& connections = net.peer(1).swarmState(1).connections;
        check(connections.size() == 1 && connections.at(regular[1]).handshakeComplete(),
            "Outgoing target not enforced independently of numwant");
        for (const auto id : regular) {
            if (id != regular[1]) rejects([&] { net.transmissionState(1, id); });
        }
    });
    at(sim, 4.5, [&] {
        check(regularCalls.size() == 2, "Full outgoing target created another path");
        // Tracker has one seeded RNG across swarms: this checks that even the
        // full-target regular announce sampled configured numwant, not zero.
        net.announceToTracker(2, 6, 1);
    });
    at(sim, 5, [&] {
        check(probeCalls == 1 && net.peer(6).swarmState(2).connections.at(probe.front()).handshakeComplete(),
            "Post-periodic tracker RNG probe failed");
    });
    leaveAt(sim, net, 5.5, 1);
    sim.run();
    check(net.activeTransmissions().empty(), "Lazy periodic transmissions leaked");
}
void trackerMembershipInspection() {
    const Swarm a(1, InfoHash{1}, 1), b(2, InfoHash{2}, 1);
    Simulation sim;
    Network net(sim, makePeers(2), {}, {a, b}, 50, 7);
    const auto& tracker = net.tracker();
    check(tracker.interval() == 7, "Network tracker interval inaccessible");
    check(tracker.registeredPeers(a.infoHash()).empty(), "Unexpected initial registrations");
    net.joinSwarm(1, 1, JoinOptions{0, 0, std::nullopt});
    net.joinSwarm(2, 1, JoinOptions{0, 0, std::nullopt});
    net.joinSwarm(1, 2, JoinOptions{0, 0, std::nullopt});
    check(tracker.registeredPeers(a.infoHash()).empty()
        && tracker.registeredPeers(b.infoHash()).empty(), "Inspection registered before STARTED");
    check(sim.currentTime() == 0 && net.links().empty(), "Inspection advanced time or created Links");
    check(sim.step(), "Missing first STARTED");
    check(tracker.registeredPeers(a.infoHash()) == std::set<PeerId>{1}
        && tracker.registeredPeers(b.infoHash()).empty(), "First STARTED registration/isolation incorrect");
    check(sim.step() && sim.step(), "Missing other STARTED events");
    check(tracker.registeredPeers(a.infoHash()) == std::set<PeerId>({1, 2})
        && tracker.registeredPeers(b.infoHash()) == std::set<PeerId>{1}, "Multi-swarm registrations missing");
    net.leaveSwarm(1, 1);
    check(tracker.registeredPeers(a.infoHash()) == std::set<PeerId>{2}
        && tracker.registeredPeers(b.infoHash()) == std::set<PeerId>{1}, "Leave leaked across swarms");
    net.leaveSwarm(1, 2);
    check(tracker.registeredPeers(a.infoHash()).empty(), "Last leave retained registration");
    net.joinSwarm(1, 1, JoinOptions{0, 0, std::nullopt});
    check(tracker.registeredPeers(a.infoHash()).empty(), "Rejoin registered before STARTED");
    check(sim.step(), "Missing rejoin STARTED");
    check(tracker.registeredPeers(a.infoHash()) == std::set<PeerId>{1}, "Rejoin not reflected");
    net.leaveSwarm(1, 1);
    net.leaveSwarm(2, 1);
    check(tracker.registeredPeers(a.infoHash()).empty()
        && tracker.registeredPeers(b.infoHash()).empty(), "Final registrations not removed");
    check(sim.currentTime() == 0 && net.links().empty(), "Queries changed time/topology");
    sim.run(); // Drain stale timers after all runtime memberships leave.
}
void runtimeJoin() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation sim(false, 7);
    Network net(sim, makePeers(2), paths(2), {swarm}, 50, 2);
    std::vector<std::tuple<double, PeerId, AnnounceKind>> announces;
    unsigned sends = 0, starts = 0, arrivals = 0;
    sim.setEventObserver([&](const Event& event) {
        if (const auto* announce = dynamic_cast<const TrackerAnnounceEvent*>(&event)) {
            if (net.lifecycleCurrent(1, announce->peerId(), announce->generation()))
                announces.emplace_back(event.time(), announce->peerId(), announce->kind());
            check(net.peer(announce->peerId()).hasSwarm(1), "Announce preceded join");
        }
        if (dynamic_cast<const SendMessageEvent*>(&event)) { ++sends; check(event.time() >= 4, "Send before late join"); }
        if (dynamic_cast<const TransmissionStartEvent*>(&event)) ++starts;
        if (dynamic_cast<const MessageArrivalEvent*>(&event)) ++arrivals;
    });
    sim.schedule(std::make_unique<PeerJoinEvent>(2, net, 1, 1));
    at(sim, 1, [&] { check(!net.peer(1).hasSwarm(1) && sends == 0, "Premature membership/activity"); });
    // numwant zero registers the late peer without initiating discovery. Existing
    // peer 1 must discover it on its t=4 regular announce (join executes first).
    sim.schedule(std::make_unique<PeerJoinEvent>(4, net, 1, 2, JoinOptions{0, 3, std::nullopt}));
    // Peer 1's t=4 timer precedes peer 2's STARTED, so its next chance is t=6.
    at(sim, 6.5, [&] {
        check(net.peer(1).swarmState(1).connections.at(2).handshakeComplete(), "Regular announce did not discover late peer");
        check(net.peer(2).swarmState(1).connections.at(1).bitfieldSent, "BITFIELD missing");
    });
    leaveAt(sim, net, 7, 1);
    leaveAt(sim, net, 7, 2);
    sim.run();
    const std::vector<std::tuple<double, PeerId, AnnounceKind>> expected{
        {2, 1, AnnounceKind::Started}, {4, 1, AnnounceKind::Regular}, {4, 2, AnnounceKind::Started},
        {6, 1, AnnounceKind::Regular}, {6, 2, AnnounceKind::Regular}};
    check(announces == expected, "Periodic timing/order or single-chain invariant failed");
    check(sends == 4 && starts == 4 && arrivals == 4, "Handshake/BITFIELD did not use normal event path exactly once");
    check(!net.peer(1).isActiveInSwarm(1) && !net.peer(2).isActiveInSwarm(1), "Leave did not deactivate");
    drained(net, 2);
}
void runtimeDownload() {
    const Swarm swarm(1, InfoHash{1}, 16384, 16384);
    Simulation sim(false, 7);
    Network net(sim, makePeers(2), paths(2), {swarm}, 50, 30);
    std::vector<std::tuple<double, PeerId, AnnounceKind>> announces;
    std::map<MessageType, double> firstArrival;
    sim.setEventObserver([&](const Event& event) {
        if (const auto* announce = dynamic_cast<const TrackerAnnounceEvent*>(&event);
            announce && net.lifecycleCurrent(1, announce->peerId(), announce->generation()))
            announces.emplace_back(event.time(), announce->peerId(), announce->kind());
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event))
            firstArrival.try_emplace(arrival->details().messageType, event.time());
    });
    net.joinSwarm(1, 1, JoinOptions{50, 1, std::vector<std::uint8_t>{0x80}});
    at(sim, 1, [&] {
        check(net.peer(1).isActiveInSwarm(1), "Runtime seeder is not active");
        check(announces == std::vector<std::tuple<double, PeerId, AnnounceKind>>{
            {0, 1, AnnounceKind::Started}}, "Seeder STARTED did not run before leecher join");
        check(!net.peer(2).hasSwarm(1) && !net.peer(2).isActiveInSwarm(1), "Leecher active before runtime join");
        check(firstArrival.empty(), "Peer-wire traffic preceded leecher join");
    });
    sim.schedule(std::make_unique<PeerJoinEvent>(2, net, 1, 2));
    auto checkDownload = [&] {
        const auto& state = net.peer(2).swarmState(1);
        check(state.localBitfield == std::vector<std::uint8_t>{0x80}, "Runtime download did not complete torrent");
        check(state.receivedBlocks.size() == 1
            && state.receivedBlocks.at(0) == std::vector<BlockRange>{{0, 16384}},
            "Runtime download lacks complete received byte coverage");
    };
    at(sim, 11, [&] {
        checkDownload();
        const auto& connection = net.peer(2).swarmState(1).connections.at(1);
        check(connection.handshakeComplete() && connection.bitfieldSent
            && connection.remoteBitfield == std::vector<std::uint8_t>{0x80}, "Runtime discovery handshake/BITFIELD incomplete");
        check(connection.scheduledRequests.empty() && connection.outgoingRequests.empty()
            && net.peer(1).swarmState(1).connections.at(2).acceptedRequests.empty(), "Completed download retained requests");
        double previous = 2;
        for (const auto type : {MessageType::Handshake, MessageType::Bitfield, MessageType::Interested,
                               MessageType::Unchoke, MessageType::Request, MessageType::Piece}) {
            check(firstArrival.contains(type) && firstArrival.at(type) > previous, "Runtime protocol flow missing or out of order");
            previous = firstArrival.at(type);
        }
    });
    leaveAt(sim, net, 12, 1);
    leaveAt(sim, net, 12, 2);
    sim.run();
    check(announces == std::vector<std::tuple<double, PeerId, AnnounceKind>>{
        {0, 1, AnnounceKind::Started}, {2, 2, AnnounceKind::Started}}, "Download did not use runtime STARTED discovery");
    check(!net.peer(1).isActiveInSwarm(1) && !net.peer(2).isActiveInSwarm(1), "Runtime memberships did not leave");
    checkDownload();
    drained(net, 2);
    check(near(sim.currentTime(), 32), "Stopped periodic chains did not drain at last stale announce");
    std::cout << "Runtime download first arrivals (seconds):";
    for (const auto& [type, name] : {std::pair{MessageType::Handshake, " HANDSHAKE="},
        {MessageType::Bitfield, " BITFIELD="}, {MessageType::Interested, " INTERESTED="},
        {MessageType::Unchoke, " UNCHOKE="}, {MessageType::Request, " REQUEST="}, {MessageType::Piece, " PIECE="}})
        std::cout << name << std::to_string(firstArrival.at(type));
    std::cout << "; drained=" << std::to_string(sim.currentTime()) << '\n';
}
void lateStarted() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation sim;
    Network net(sim, makePeers(2), paths(2), {swarm}, 50, 10);
    net.joinSwarm(1, 1);
    sim.schedule(std::make_unique<PeerJoinEvent>(3, net, 1, 2));
    at(sim, 3.5, [&] { check(net.peer(2).swarmState(1).connections.at(1).handshakeComplete(), "Late STARTED discovery failed"); });
    leaveAt(sim, net, 4, 1); leaveAt(sim, net, 4, 2);
    sim.run();
}
void validation() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation sim;
    for (double interval : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { Network invalid(sim, makePeers(1), {}, {swarm}, 50, interval); });
    Network net(sim, makePeers(1), {}, {swarm}, 50, 2);
    rejects([&] { net.joinSwarm(1, 1, JoinOptions{50, 8, std::vector<std::uint8_t>{1}}); });
    check(!net.peer(1).hasSwarm(1), "Invalid initial bitfield mutated membership");
    net.joinSwarm(1, 1, JoinOptions{50, 2, std::vector<std::uint8_t>{0x80}});
    const auto generation = net.lifecycleGeneration(1, 1);
    rejects([&] { net.joinSwarm(1, 1); });
    check(net.lifecycleGeneration(1, 1) == generation && net.peer(1).swarmState(1).localBitfield[0] == 0x80,
          "Duplicate join mutated membership");
    net.leaveSwarm(1, 1); net.leaveSwarm(1, 1);
    check(net.lifecycleGeneration(1, 1) == generation + 1, "Repeated leave changed generation");
    rejects([&] { net.joinSwarm(1, 1, JoinOptions{0, 0, std::vector<std::uint8_t>{0}}); });
    check(!net.peer(1).isActiveInSwarm(1), "Rejected rejoin activated membership");
    net.joinSwarm(1, 1, JoinOptions{0, 0, std::nullopt});
    check(net.peer(1).swarmState(1).localBitfield[0] == 0x80, "Rejoin lost data");
    leaveAt(sim, net, 1, 1);
    sim.run();
}
void trackerStopped() {
    Tracker tracker(50, 123, 2), baseline(50, 123, 2);
    const InfoHash a{1}, b{2};
    for (auto* t : {&tracker, &baseline}) {
        t->announce(a, 1, 0, AnnounceKind::Started);
        t->announce(a, 2, 0, AnnounceKind::Regular);
        t->announce(b, 1, 0, AnnounceKind::Started);
    }
    check(tracker.announce(a, 1, 100, AnnounceKind::Stopped).empty(), "Stopped returned peers");
    tracker.announce(a, 1, 100, AnnounceKind::Stopped);
    check(tracker.announce(a, 2, 100).empty(), "Stopped registration retained");
    check(tracker.announce(b, 3, 100) == baseline.announce(b, 3, 100), "Stopped leaked hash or consumed RNG");
}
void outgoingCapacityReuse() {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation sim(false, 7);
    Network net(sim, makePeers(4), paths(4), {swarm}, 2, 1);
    net.joinSwarm(1, 2, JoinOptions{0, 0, std::nullopt});
    net.joinSwarm(1, 3, JoinOptions{0, 0, std::nullopt});
    sim.schedule(std::make_unique<PeerJoinEvent>(.1, net, 1, 1, JoinOptions{2, 2, std::nullopt}));
    at(sim, .5, [&] {
        const auto& connections = net.peer(1).swarmState(1).connections;
        check(connections.size() == 2 && connections.at(2).handshakeComplete()
            && connections.at(3).handshakeComplete(), "Initial outgoing relationships incomplete");
        rejects([&] { net.setTargetOutgoingConnections(1, 1, 1); });
    });
    sim.schedule(std::make_unique<PeerJoinEvent>(1.5, net, 1, 4, JoinOptions{0, 0, std::nullopt}));
    leaveAt(sim, net, 2, 2);
    at(sim, 2.01, [&] {
        check(!net.peer(1).swarmState(1).connections.contains(2), "Departed relationship retained");
        net.setTargetOutgoingConnections(1, 1, 1); // Exactly one outgoing remains.
        rejects([&] { net.setTargetOutgoingConnections(1, 1, 0); });
        net.setTargetOutgoingConnections(1, 1, 2);
    });
    at(sim, 8, [&] {
        const auto& connections = net.peer(1).swarmState(1).connections;
        check(connections.size() == 2 && connections.at(4).handshakeComplete(), "Regular announce did not replace departed source");
        rejects([&] { net.setTargetOutgoingConnections(1, 1, 1); });
        net.setTargetOutgoingConnections(1, 4, 0); // Replacement is incoming at D.
    });
    for (PeerId id : {1u, 3u, 4u}) leaveAt(sim, net, 9, id);
    at(sim, 9.1, [&] {
        net.joinSwarm(1, 1, JoinOptions{0, 0, std::nullopt});
        net.setTargetOutgoingConnections(1, 1, 0); // Rejoin starts without old ownership.
        check(net.peer(1).swarmState(1).connections.empty(), "Rejoin retained connections");
    });
    leaveAt(sim, net, 9.2, 1);
    sim.run();
    drained(net, 4);
}
void lateJoinFilledOutgoingTargets() {
    const Swarm swarm(1, InfoHash{1}, 16384, 16384);
    auto peers = makePeers(9);
    std::vector<Link> links;
    for (PeerId id = 1; id <= 8; ++id) {
        peers[id - 1].joinSwarm(swarm, {0x80});
        links.emplace_back(id, id == 8 ? 1 : id + 1, 1000000, .01);
        links.emplace_back(id, 9, 1000000, .01);
    }
    Simulation sim(false, 7);
    Network net(sim, peers, links, {swarm}, 2, 5);
    for (PeerId id = 1; id <= 8; ++id) net.setTargetOutgoingConnections(1, id, 1);
    net.announceToTracker(1, 1, 0);
    // Build a sparse directed ring through real tracker responses and handshakes.
    // Register the next endpoint before each initiator; repeated small samples
    // skip unavailable paths and already-admitted relationships normally.
    for (PeerId id = 1; id <= 8; ++id) {
        if (id < 8) net.announceToTracker(1, id + 1, 0);
        for (unsigned sample = 0; sample < 32; ++sample) net.announceToTracker(1, id, 2);
    }
    auto checkExisting = [&] {
        for (PeerId id = 1; id <= 8; ++id) {
            check(net.peer(id).isActiveInSwarm(1), "Existing peer inactive");
            const auto& connections = net.peer(id).swarmState(1).connections;
            check(connections.at(id == 8 ? 1 : id + 1).handshakeComplete(), "Tracker-built ring incomplete");
            rejects([&] { net.setTargetOutgoingConnections(1, id, 0); });
        }
    };
    at(sim, 1, [&] {
        checkExisting();
        for (PeerId id = 1; id <= 8; ++id)
            check(net.peer(id).swarmState(1).connections.size() == 2, "Startup overlay is not a sparse ring");
        check(!net.peer(9).isActiveInSwarm(1), "Late peer joined early");
    });
    bool started = false;
    unsigned pieceArrivals = 0;
    std::set<PeerId> pieceProviders;
    sim.setEventObserver([&](const Event& event) {
        if (const auto* announce = dynamic_cast<const TrackerAnnounceEvent*>(&event);
            announce && announce->peerId() == 9 && announce->kind() == AnnounceKind::Started) {
            check(near(event.time(), 2), "Late STARTED time changed");
            started = true;
        }
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event);
            arrival && arrival->details().receiver == 9 && arrival->details().messageType == MessageType::Piece) {
            check(pieceProviders.insert(arrival->details().sender).second, "Repeated PIECE from the same provider");
            if (pieceArrivals > 0)
                check(net.peer(9).swarmState(1).connections.at(arrival->details().sender).retiredRequests
                    == std::vector<RequestPayload>{{0, 0, 16384}}, "Late PIECE was not legitimately retired");
            ++pieceArrivals;
        }
    });
    at(sim, 2, [&] { net.joinSwarm(1, 9, JoinOptions{2, 2, std::nullopt}); });
    at(sim, 3, [&] {
        checkExisting();
        const auto& connections = net.peer(9).swarmState(1).connections;
        check(started && connections.size() == 2, "Late peer did not enter overlay through STARTED");
        rejects([&] { net.setTargetOutgoingConnections(1, 9, 1); });
        for (const auto& [remote, connection] : connections) {
            check(connection.handshakeComplete() && connection.bitfieldSent
                && connection.remoteBitfield == std::vector<std::uint8_t>{0x80}, "Late peer protocol discovery incomplete");
            net.setTargetOutgoingConnections(1, remote, 1); // Incoming did not add ownership.
            check(net.peer(remote).swarmState(1).connections.size() == 3, "Full outgoing peer rejected incoming");
        }
    });
    at(sim, 12, [&] {
        const auto& state = net.peer(9).swarmState(1);
        check(pieceArrivals >= 1 && pieceArrivals <= state.connections.size() && state.localBitfield == std::vector<std::uint8_t>{0x80}
            && state.receivedBlocks.at(0) == std::vector<BlockRange>{{0, 16384}}, "Late peer failed automatic download");
    });
    for (PeerId id = 1; id <= 9; ++id) leaveAt(sim, net, 13, id);
    sim.run();
    check(net.activeTransmissions().empty(), "Late-join transfer did not drain");
}
void staleTimers() {
    auto scenario = [](bool inject) {
        Swarm swarm(1, InfoHash{1}, 1);
        Simulation sim(false, 19);
        Network net(sim, makePeers(5), paths(5), {swarm}, 2, 2);
        for (PeerId id = 2; id <= 5; ++id) net.joinSwarm(1, id, JoinOptions{0, 3, std::nullopt});
        net.joinSwarm(1, 1, JoinOptions{0, 3, std::nullopt});
        if (inject) {
            sim.schedule(std::make_unique<TrackerAnnounceEvent>(3.1, net, 1, 1, 50));
            sim.schedule(std::make_unique<RechokeEvent>(3.1, net, 1, 1));
        }
        leaveAt(sim, net, 1, 1);
        sim.schedule(std::make_unique<PeerJoinEvent>(3, net, 1, 1, JoinOptions{2, 3, std::nullopt}));
        unsigned validRegular = 0;
        sim.setEventObserver([&](const Event& event) {
            if (const auto* announce = dynamic_cast<const TrackerAnnounceEvent*>(&event); announce
                && announce->peerId() == 1 && announce->kind() == AnnounceKind::Regular
                && net.lifecycleCurrent(1, 1, announce->generation()) && event.time() >= 3)
                ++validRegular;
        });
        std::set<PeerId> result;
        at(sim, 3.5, [&] {
            for (const auto& [remote, c] : net.peer(1).swarmState(1).connections) result.insert(remote);
            check(!net.peer(1).swarmState(1).choking.managed, "Stale rechoke mutated rejoined state");
        });
        for (PeerId id = 1; id <= 5; ++id) leaveAt(sim, net, 6, id);
        sim.run();
        check(validRegular == 1, "Stale timer disrupted new periodic chain");
        return result;
    };
    check(scenario(true) == scenario(false), "Stale timers changed seeded response or rejoined membership");
}
void handshakeLeave(bool propagating) {
    Swarm swarm(1, InfoHash{1}, 1);
    Simulation sim;
    Network net(sim, makePeers(2), paths(2, 1000000, 1), {swarm}, 50, 10);
    net.joinSwarm(1, 1); net.joinSwarm(1, 2);
    bool triggered = false;
    sim.setEventObserver([&](const Event& event) {
        if (triggered) return;
        if (!propagating) {
            const auto* send = dynamic_cast<const SendMessageEvent*>(&event);
            if (!send || send->details().messageType != MessageType::Handshake) return;
            triggered = true;
            PeerLeaveEvent leave(sim.currentTime(), net, 1, 1); sim.executeNow(leave);
        } else {
            const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event);
            if (!complete || !complete->details() || complete->details()->messageType != MessageType::Handshake) return;
            triggered = true;
            leaveAt(sim, net, sim.currentTime(), 1);
        }
        sim.schedule(std::make_unique<PeerJoinEvent>(sim.currentTime() + .1, net, 1, 1, JoinOptions{0, 0, std::nullopt}));
    });
    leaveAt(sim, net, 3, 1); leaveAt(sim, net, 3, 2);
    at(sim, 2, [&] {
        check(net.peer(1).swarmState(1).connections.empty() && net.peer(2).swarmState(1).connections.empty(),
              "Stale handshake resurrected connection after rejoin");
    });
    sim.run();
    check(triggered, "Handshake phase not exercised");
    drained(net, 2);
}
void sharedBandwidth() {
    Swarm a(1, InfoHash{1}, 1), b(2, InfoHash{2}, 1);
    auto peers = makePeers(3, 100);
    for (auto& p : peers) { p.joinSwarm(a); p.joinSwarm(b); }
    Simulation sim;
    Network net(sim, peers, paths(3, 100, .01), {a, b});
    handshake(sim, net, a, 1, 2); handshake(sim, net, b, 1, 3);
    leaveAt(sim, net, 1, 1, 1);
    at(sim, 1, [&] {
        check(net.activeTransmissions().size() == 1, "Canceled active flow retained");
        const auto& active = net.activeTransmissions().begin()->second;
        check(active.swarmId == 2 && near(active.remainingBits, 494) && near(active.currentRate, 100), "Old-rate progress/reallocation wrong");
    });
    bool arrival = false;
    sim.setEventObserver([&](const Event& event) {
        if (const auto* e = dynamic_cast<const MessageArrivalEvent*>(&event); e && e->details().sender == 1
            && e->details().receiver == 3 && e->details().messageType == MessageType::Handshake) {
            check(near(event.time(), 5.95), "Surviving flow timing changed incorrectly"); arrival = true;
        }
    });
    sim.run();
    check(arrival && net.peer(1).swarmState(2).connections.at(3).handshakeComplete(), "Other swarm stopped");
    drained(net, 3);
}
void replacementCompletion() {
    Swarm a(1, InfoHash{1}, 1), b(2, InfoHash{2}, 1);
    auto peers = makePeers(2, 100);
    for (auto& p : peers) { p.joinSwarm(a); p.joinSwarm(b); }
    Simulation sim;
    Network net(sim, peers, paths(2, 100), {a, b});
    handshake(sim, net, a, 1, 2); handshake(sim, net, b, 1, 2);
    leaveAt(sim, net, 1, 1, 1);
    at(sim, 5.45, [&] {
        check(net.transmissionState(1, 2).active, "Old completion released replacement FIFO direction");
        check(net.activeTransmissions().size() == 1 && net.activeTransmissions().begin()->second.swarmId == 2,
              "Old completion removed replacement record");
    });
    sim.run();
    check(net.peer(1).swarmState(2).connections.at(2).handshakeComplete(), "Replacement handshake failed");
    drained(net, 2);
}
enum class LeavePhase { ScheduledRequest, QueuedRequest, ScheduledPiece, QueuedPiece, ActivePiece, PropagatingPiece };
void transferLeave(LeavePhase phase) {
    const Swarm swarm(1, InfoHash{1}, 6 * 16384, 6 * 16384);
    auto peers = makePeers(2);
    peers[0].joinSwarm(swarm, {0x80}); peers[1].joinSwarm(swarm);
    Simulation sim;
    Network net(sim, peers, paths(2), {swarm}, 50, 100);
    handshake(sim, net, swarm, 2, 1);
    bool triggered = false, sawQueued = false;
    sim.setEventObserver([&](const Event& event) {
        if (const auto* leave = dynamic_cast<const PeerLeaveEvent*>(&event); leave && leave->peerId() == 1
            && (phase == LeavePhase::QueuedRequest || phase == LeavePhase::QueuedPiece)) {
            const PeerId sender = phase == LeavePhase::QueuedRequest ? 2 : 1;
            sawQueued |= net.transmissionState(sender, sender == 1 ? 2 : 1).pendingCount > 0;
        }
        if (triggered) return;
        bool match = false, immediate = false;
        if (const auto* send = dynamic_cast<const SendMessageEvent*>(&event)) {
            const auto type = send->details().messageType;
            if (phase == LeavePhase::ScheduledRequest && type == MessageType::Request) match = immediate = true;
            if (phase == LeavePhase::ScheduledPiece && type == MessageType::Piece) match = immediate = true;
            if (phase == LeavePhase::QueuedPiece && type == MessageType::Piece && net.transmissionState(1, 2).active) match = true;
        }
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event)) {
            if (phase == LeavePhase::QueuedRequest && start->details().messageType == MessageType::Request) match = true;
            if (phase == LeavePhase::ActivePiece && start->details().messageType == MessageType::Piece) match = true;
        }
        if (const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
            if (phase == LeavePhase::PropagatingPiece && complete->details()
                && complete->details()->messageType == MessageType::Piece) match = true;
        }
        if (!match) return;
        triggered = true;
        const double when = sim.currentTime() + (phase == LeavePhase::ActivePiece ? .001 : 0);
        if (immediate) { PeerLeaveEvent leave(when, net, 1, 1); sim.executeNow(leave); }
        else leaveAt(sim, net, when, 1);
        // Rejoin before old arrivals/completions fire; no new connections permitted.
        sim.schedule(std::make_unique<PeerJoinEvent>(when + .00001, net, 1, 1, JoinOptions{0, 0, std::nullopt}));
        at(sim, when + .02, [&] {
            check(net.peer(2).swarmState(1).connections.empty(), "Request connection/reservations survived leave");
            check(net.peer(2).swarmState(1).receivedBlocks.empty(), "Stale PIECE credited received bytes");
            check(net.peer(2).swarmState(1).localBitfield[0] == 0, "Stale PIECE completed data");
            check(net.peer(1).swarmState(1).connections.empty(), "Pre-leave event affected rejoined membership");
        });
        leaveAt(sim, net, when + 1, 1);
    });
    sim.run();
    check(triggered, "Requested transfer leave phase not reached");
    if (phase == LeavePhase::QueuedRequest || phase == LeavePhase::QueuedPiece) check(sawQueued, "FIFO phase had no queued message");
    check(net.peer(2).swarmState(1).receivedBlocks.empty(), "Late stale PIECE credited data");
    check(net.peer(2).swarmState(1).connections.empty(), "Departed connection retained byte-credit state");
    drained(net, 2);
}
void recovery() {
    const Swarm swarm(1, InfoHash{1}, 6 * 16384, 6 * 16384);
    auto peers = makePeers(3);
    peers[0].joinSwarm(swarm, {0x80}); peers[1].joinSwarm(swarm); peers[2].joinSwarm(swarm, {0x80});
    Simulation sim;
    Network net(sim, peers, paths(3), {swarm});
    handshake(sim, net, swarm, 2, 1); handshake(sim, net, swarm, 2, 3);
    bool left = false, recovered = false;
    std::set<std::uint32_t> abandoned;
    sim.setEventObserver([&](const Event& event) {
        const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
        if (!start) return;
        const auto details = start->details();
        if (!left && details.messageType == MessageType::Piece && details.sender == 1) {
            const auto& pending = net.peer(2).swarmState(1).connections.at(1).outgoingRequests;
            for (const auto& request : pending) abandoned.insert(request.begin);
            check(!abandoned.empty(), "No abandoned reservations to recover");
            left = true;
            leaveAt(sim, net, sim.currentTime(), 1);
        }
        if (left && details.messageType == MessageType::Request && details.receiver == 3) {
            const auto block = std::get<RequestPayload>(start->message().payload());
            if (abandoned.contains(block.begin)) recovered = true;
        }
    });
    sim.run();
    check(left && recovered, "Abandoned block not requested from surviving eligible neighbor");
    check(net.peer(2).swarmState(1).localBitfield[0] == 0x80, "Recovery did not complete download");
    check(!net.peer(2).swarmState(1).connections.contains(1), "Departed source still contributes availability");
    const auto& connection = net.peer(2).swarmState(1).connections.at(3);
    check(connection.scheduledRequests.empty() && connection.outgoingRequests.empty(), "Recovery leaked reservations");
    drained(net, 3);
}
void downloadedRejoin() {
    const Swarm swarm(1, InfoHash{1}, 2 * 16384, 16384);
    auto peers = makePeers(2);
    peers[0].joinSwarm(swarm, {0xc0}); peers[1].joinSwarm(swarm);
    Simulation sim;
    Network net(sim, peers, paths(2, 1000000, .1), {swarm}, 50, 1);
    handshake(sim, net, swarm, 2, 1);
    bool left = false;
    std::vector<std::uint8_t> saved;
    std::unordered_map<std::uint32_t, std::vector<BlockRange>> ranges;
    std::uint64_t oldGeneration = 0;
    sim.setEventObserver([&](const Event& event) {
        const auto* send = dynamic_cast<const SendMessageEvent*>(&event);
        if (left || !send || send->details().messageType != MessageType::Have || send->details().sender != 2) return;
        left = true;
        saved = net.peer(2).swarmState(1).localBitfield;
        ranges = net.peer(2).swarmState(1).receivedBlocks;
        check(saved[0] == 0x80, "Expected one downloaded piece before leave");
        oldGeneration = net.lifecycleGeneration(1, 2);
        PeerLeaveEvent leave(sim.currentTime(), net, 1, 2); sim.executeNow(leave);
        sim.schedule(std::make_unique<PeerJoinEvent>(sim.currentTime() + .00001, net, 1, 2, JoinOptions{0, 0, std::nullopt}));
        at(sim, sim.currentTime() + .5, [&] {
            const auto& state = net.peer(2).swarmState(1);
            check(state.localBitfield == saved && state.receivedBlocks == ranges, "Rejoin lost data or stale PIECE changed it");
            check(state.connections.empty() && !state.choking.managed, "Rejoin retained protocol/policy state");
            check(state.lifecycleGeneration == oldGeneration + 2, "Rejoin did not advance generation");
        });
        leaveAt(sim, net, sim.currentTime() + .6, 2);
    });
    sim.run();
    check(left, "Download/rejoin scenario never acquired data");
    check(net.peer(2).swarmState(1).localBitfield == saved, "Old arrival mutated inactive saved data");
    drained(net, 2);
}
void staleMessages() {
    Swarm swarm(1, InfoHash{1}, 1);
    auto peers = makePeers(2);
    for (auto& p : peers) p.joinSwarm(swarm);
    Simulation sim;
    Network net(sim, peers, paths(2), {swarm}, 50, 10);
    // All are intentionally invalid for a fresh connection, but stale generation
    // must discard them before protocol lookups/validation or request mutations.
    std::vector<Message> messages{
        Message(MessageType::Handshake, HandshakePayload{swarm.infoHash(), peers[0].protocolId()}),
        Message(MessageType::Bitfield, BitfieldPayload{{0}}), Message(MessageType::Have, HavePayload{0}),
        Message(MessageType::Choke), Message(MessageType::Unchoke), Message(MessageType::Interested),
        Message(MessageType::NotInterested), Message(MessageType::Request, RequestPayload{0, 0, 1}),
        Message(MessageType::Piece, PiecePayload{0, 0, 1}), Message(MessageType::Cancel, CancelPayload{0, 0, 1})};
    for (const auto& message : messages) {
        sim.schedule(std::make_unique<SendMessageEvent>(2, net, 1, 1, 2, message));
        sim.schedule(std::make_unique<MessageArrivalEvent>(2, net, 1, 1, 2, message));
        sim.schedule(std::make_unique<TransmissionStartEvent>(2, net, 1, 1, 2, message));
    }
    sim.schedule(std::make_unique<SendMessageEvent>(2, net, 1, 1, 2,
        Message(MessageType::Request, RequestPayload{0, 0, 1}), true));
    sim.schedule(std::make_unique<SendMessageEvent>(2, net, 1, 1, 2,
        Message(MessageType::Piece, PiecePayload{0, 0, 1}), false, true));
    leaveAt(sim, net, .5, 2);
    sim.schedule(std::make_unique<PeerJoinEvent>(1, net, 1, 2, JoinOptions{0, 0, std::nullopt}));
    at(sim, 3, [&] { check(net.peer(2).swarmState(1).connections.empty(), "Stale messages recreated protocol state"); });
    leaveAt(sim, net, 4, 2);
    sim.run();
    drained(net, 2);
}
void optimisticLeave() {
    Swarm swarm(1, InfoHash{1}, 6 * 16384, 6 * 16384);
    auto peers = makePeers(7, 1000);
    peers[0].joinSwarm(swarm, {0x80});
    for (unsigned i = 1; i < peers.size(); ++i) peers[i].joinSwarm(swarm);
    Simulation sim;
    Network net(sim, peers, paths(7, 1000), {swarm});
    for (PeerId id = 2; id <= 7; ++id) handshake(sim, net, swarm, id, 1);
    PeerId departed = 0;
    at(sim, 10.5, [&] {
        const auto optimistic = net.peer(1).swarmState(1).choking.optimistic;
        check(optimistic.has_value(), "Optimistic connection not established for cleanup test");
        departed = *optimistic;
        PeerLeaveEvent leave(sim.currentTime(), net, 1, departed); sim.executeNow(leave);
        const auto& state = net.peer(1).swarmState(1);
        check(!state.choking.optimistic && state.choking.optimisticCursor != departed, "Dangling optimistic reference");
    });
    at(sim, 20.5, [&] {
        const auto& state = net.peer(1).swarmState(1);
        check(departed != 0 && !state.connections.contains(departed), "Departed optimistic connection resurrected");
        if (state.choking.optimistic) check(state.connections.contains(*state.choking.optimistic), "Rechoke selected erased connection");
    });
    for (PeerId id = 1; id <= 7; ++id) leaveAt(sim, net, 21, id);
    sim.run();
    drained(net, 7);
}
void otherSwarmTransfer() {
    Swarm a(1, InfoHash{1}, 2 * 16384, 16384), b(2, InfoHash{2}, 2 * 16384, 16384);
    auto peers = makePeers(2);
    peers[0].joinSwarm(a, {0xc0}); peers[0].joinSwarm(b, {0xc0});
    peers[1].joinSwarm(a); peers[1].joinSwarm(b);
    Simulation sim;
    Network net(sim, peers, paths(2), {a, b});
    handshake(sim, net, a, 2, 1); handshake(sim, net, b, 2, 1);
    bool left = false;
    sim.setEventObserver([&](const Event& event) {
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event); !left && start
            && start->details().swarmId == 1 && start->details().messageType == MessageType::Piece) {
            left = true; leaveAt(sim, net, sim.currentTime(), 1, 1);
        }
    });
    sim.run();
    check(left && net.peer(2).swarmState(2).localBitfield[0] == 0xc0, "Other swarm transfer interrupted");
    check(net.peer(2).swarmState(1).receivedBlocks.empty(), "Canceled swarm credited PIECE");
    check(net.peer(2).swarmState(2).connections.at(1).outgoingRequests.empty(), "Other swarm request leaked");
    drained(net, 2);
}
void sameTimeLeave(bool beforeArrival) {
    auto scenario = [=] {
        Swarm swarm(1, InfoHash{1}, 16384, 16384);
        auto peers = makePeers(2);
        peers[0].joinSwarm(swarm, {0x80}); peers[1].joinSwarm(swarm);
        Simulation sim;
        Network net(sim, peers, paths(2), {swarm});
        handshake(sim, net, swarm, 2, 1);
        bool left = false;
        std::vector<std::pair<double, std::string>> trace;
        sim.setEventObserver([&](const Event& event) {
            trace.emplace_back(event.time(), event.traceDescription());
            if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event); !left && arrival
                && arrival->details().messageType == MessageType::Piece) {
                left = true;
                if (beforeArrival) { PeerLeaveEvent leave(sim.currentTime(), net, 1, 2); sim.executeNow(leave); }
                else leaveAt(sim, net, sim.currentTime(), 2);
            }
        });
        sim.run();
        check(left, "Same-time PIECE arrival not reached");
        check(net.peer(2).swarmState(1).localBitfield[0] == (beforeArrival ? 0 : 0x80), "Same-time leave/arrival ordering violated");
        return trace;
    };
    check(scenario() == scenario(), "Same-time lifecycle trace not reproducible");
}
void partialDataRejoin() {
    Swarm swarm(1, InfoHash{1}, 4 * 16384, 4 * 16384);
    auto peers = makePeers(2);
    peers[0].joinSwarm(swarm, {0x80}); peers[1].joinSwarm(swarm);
    Simulation sim;
    Network net(sim, peers, paths(2), {swarm}, 50, 1);
    handshake(sim, net, swarm, 2, 1);
    bool triggered = false;
    sim.setEventObserver([&](const Event& event) {
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event); !triggered && arrival
            && arrival->details().messageType == MessageType::Piece) {
            triggered = true;
            at(sim, sim.currentTime(), [&] {
                const auto saved = net.peer(2).swarmState(1).receivedBlocks;
                check(!saved.empty() && net.peer(2).swarmState(1).localBitfield[0] == 0, "Expected partial piece data");
                net.leaveSwarm(1, 2);
                net.joinSwarm(1, 2, JoinOptions{0, 0, std::nullopt});
                at(sim, sim.currentTime() + .5, [&, saved] {
                    check(net.peer(2).swarmState(1).receivedBlocks == saved, "Partial blocks lost or stale data accepted after rejoin");
                    check(net.peer(2).swarmState(1).localBitfield[0] == 0, "Stale PIECE completed partial data");
                });
                leaveAt(sim, net, sim.currentTime() + .6, 2);
            });
        }
    });
    sim.run();
    check(triggered, "Partial data scenario not exercised");
    drained(net, 2);
}
}
int main() {
    const std::pair<const char*, void(*)()> tests[] = {
        {"Read-only tracker registration inspection follows multi-swarm join/leave/rejoin", trackerMembershipInspection},
        {"Periodic numwant is independent of outgoing slots, including full-target sampling", independentPeriodicNumwant},
        {"Runtime join, observability, periodic discovery and exact single-chain timing", runtimeJoin},
        {"Runtime join through STARTED discovery completes automatic download and drains", runtimeDownload},
        {"Late STARTED discovers existing peer", lateStarted},
        {"Join validation, duplicate rejection, repeated leave and preserved initial data", validation},
        {"Tracker STOPPED removal, hash isolation and RNG preservation", trackerStopped},
        {"Departed initiated neighbor frees capacity for periodic discovery and rejoin", outgoingCapacityReuse},
        {"Late runtime join downloads through peers with filled outgoing targets", lateJoinFilledOutgoingTargets},
        {"Stale tracker/rechoke timers after rejoin", staleTimers},
        {"Leave before pending handshake send", [] { handshakeLeave(false); }},
        {"Leave during propagating handshake and rejoin", [] { handshakeLeave(true); }},
        {"Cancellation accounts old rates and reallocates shared bandwidth across swarms", sharedBandwidth},
        {"Stale completion cannot release replacement transmission", replacementCompletion},
        {"Leave during scheduled REQUEST", [] { transferLeave(LeavePhase::ScheduledRequest); }},
        {"Leave during FIFO REQUEST", [] { transferLeave(LeavePhase::QueuedRequest); }},
        {"Leave during scheduled PIECE", [] { transferLeave(LeavePhase::ScheduledPiece); }},
        {"Leave during FIFO PIECE", [] { transferLeave(LeavePhase::QueuedPiece); }},
        {"Leave during active PIECE", [] { transferLeave(LeavePhase::ActivePiece); }},
        {"Leave during propagating PIECE", [] { transferLeave(LeavePhase::PropagatingPiece); }},
        {"Abandoned request reservations recover through surviving source", recovery},
        {"Downloaded data survives rejoin; propagating old PIECE cannot mutate it", downloadedRejoin},
        {"All stale peer-wire events are harmless before validation", staleMessages},
        {"Departed optimistic peer is removed before next policy decision", optimisticLeave},
        {"Other swarm completes transfer on same physical Link", otherSwarmTransfer},
        {"Same-time leave before PIECE arrival is deterministic", [] { sameTimeLeave(true); }},
        {"Same-time leave after PIECE arrival preserves delivered data", [] { sameTimeLeave(false); }},
        {"Partial downloaded ranges survive immediate leave/rejoin", partialDataRejoin}
    };
    unsigned failed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL: " << name << ": " << e.what() << '\n'; }
    }
    std::cout << "Tests: " << std::size(tests) << ", failed: " << failed << '\n';
    return failed ? 1 : 0;
}
