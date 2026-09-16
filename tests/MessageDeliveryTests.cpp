#include <cmath>
#include <source_location>
#include <limits>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "simulator/Network.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/TransmissionStartEvent.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
#include "simulator/MessageArrivalEvent.hpp"

using namespace simulator;

void check(bool condition, const std::source_location location = std::source_location::current())
{
    if (!condition) throw std::runtime_error("Message delivery check failed at line " + std::to_string(location.line()));
}

template<class Action>
void rejects(Action action)
{
    try { action(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected std::invalid_argument");
}

const PeerProtocolId testSenderProtocolId{0xa1, 0x37, 0x59};

Message handshake(const Swarm& swarm, const PeerProtocolId& id = testSenderProtocolId)
{
    return Message(MessageType::Handshake, HandshakePayload{swarm.infoHash(), id});
}

void establishHandshake(Peer& receiver, const Swarm& swarm)
{
    receiver.markHandshakeSent(swarm, 1);
    receiver.receiveMessage(swarm, 1, handshake(swarm), &testSenderProtocolId);
}

int existingDeliveryRegression()
{
    try {
        const Swarm swarm(1, {}, 9);
        const Swarm other(2, {}, 8);
        Peer receiver(2, 500, 500);
        rejects([&] { receiver.receiveMessage(swarm, 1, Message(MessageType::Unchoke)); });
        receiver.joinSwarm(swarm);
        receiver.joinSwarm(other);
        auto receive = [&](Message message) {
            receiver.receiveMessage(swarm, 1, message, &testSenderProtocolId);
        };
        auto remote = [&]() -> const PeerConnectionState& {
            return receiver.swarmState(1).connections.at(1);
        };

        rejects([&] { receive(Message(MessageType::Have, HavePayload{9})); });
        rejects([&] { receive(Message(MessageType::Bitfield, BitfieldPayload{{}})); });
        check(receiver.swarmState(1).connections.empty());
        establishHandshake(receiver, swarm);
        receive(Message(MessageType::Choke));
        check(remote().remoteIsChokingUs && !remote().remoteInterestedInUs);
        check(remote().remoteBitfield == std::vector<std::uint8_t>({0, 0}));
        receive(Message(MessageType::Unchoke));
        check(!remote().remoteIsChokingUs);
        receive(Message(MessageType::Choke));
        check(remote().remoteIsChokingUs);
        receive(Message(MessageType::Interested));
        check(remote().remoteInterestedInUs);
        receive(Message(MessageType::NotInterested));
        check(!remote().remoteInterestedInUs);

        for (auto index : {0u, 7u, 8u, 8u}) {
            receive(Message(MessageType::Have, HavePayload{index}));
        }
        check(remote().remoteBitfield == std::vector<std::uint8_t>({0x81, 0x80}));
        for (auto index : {9u, 0xffffffffu}) {
            rejects([&] { receive(Message(MessageType::Have, HavePayload{index})); });
        }
        for (const auto& bytes : {std::vector<std::uint8_t>{0},
                                 std::vector<std::uint8_t>{0, 0, 0},
                                 std::vector<std::uint8_t>{0, 1},
                                 std::vector<std::uint8_t>{0, 0x40}}) {
            rejects([&] { receive(Message(MessageType::Bitfield, BitfieldPayload{bytes})); });
        }
        check(remote().remoteBitfield == std::vector<std::uint8_t>({0x81, 0x80}));
        receive(Message(MessageType::Bitfield, BitfieldPayload{{0x40, 0}}));
        check(remote().remoteBitfield == std::vector<std::uint8_t>({0x40, 0}));
        check(receiver.swarmState(2).connections.empty());
        establishHandshake(receiver, other);
        receiver.receiveMessage(other, 1, Message(MessageType::Bitfield, BitfieldPayload{{0xff}}));
        check(receiver.swarmState(2).connections.at(1).remoteBitfield[0] == 0xff);
        for (const auto& message : {
                handshake(swarm),
                Message(MessageType::Cancel, CancelPayload{}),
                Message(MessageType::Cancel, CancelPayload{}),
                Message(MessageType::Cancel, CancelPayload{})}) {
            receive(message);
        }
        check(remote().remoteIsChokingUs && !remote().remoteInterestedInUs);
        check(remote().remoteBitfield == std::vector<std::uint8_t>({0x40, 0}));

        Peer sender(1, 1000, 1000, testSenderProtocolId);
        sender.joinSwarm(swarm);
        Simulation simulation;
        Network network(simulation, {sender, receiver}, {Link(1, 2, 800, 0.1)}, {swarm, other});
        rejects([&] { network.deliver(99, 1, 2, Message(MessageType::Unchoke)); });
        rejects([&] { network.deliver(1, 1, 99, Message(MessageType::Unchoke)); });
        rejects([&] { network.deliver(2, 2, 1, Message(MessageType::Unchoke)); });
        simulation.schedule(std::make_unique<SendMessageEvent>(
            2.0, network, 1, 1, 2, Message(MessageType::Unchoke)));
        check(network.peer(2).swarmState(1).connections.at(1).remoteIsChokingUs);
        simulation.run();
        check(!network.peer(2).swarmState(1).connections.at(1).remoteIsChokingUs);
        check(std::abs(simulation.currentTime() - 2.18) < 1e-12);
        check(network.peer(2).swarmState(2).connections.at(1).remoteIsChokingUs);
        std::cout << "Message delivery tests passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

namespace {
struct Fixture {
    Swarm swarm{1, {}, 9};
    Peer receiver{2, 500, 500};

    explicit Fixture(bool ready = true) {
        receiver.joinSwarm(swarm);
        if (ready) establishHandshake(receiver, swarm);
    }
    void receive(Message message, PeerId sender = 1) {
        receiver.receiveMessage(swarm, sender, message);
    }
    const PeerConnectionState& remote() const {
        return receiver.swarmState(swarm.id()).connections.at(1);
    }
    void seed() {
        receive(Message(MessageType::Bitfield, BitfieldPayload{{0xa5, 0x80}}));
        receive(Message(MessageType::Unchoke));
        receive(Message(MessageType::Interested));
    }
};

void checkUnchanged(const PeerSwarmState& actual, const PeerSwarmState& before)
{
    check(actual.localBitfield == before.localBitfield);
    check(actual.receivedBlocks == before.receivedBlocks);
    check(actual.connections.size() == before.connections.size());
    for (const auto& [id, expected] : before.connections) {
        const auto& connection = actual.connections.at(id);
        check(connection.handshakeSent == expected.handshakeSent);
        check(connection.handshakeReceived == expected.handshakeReceived);
        check(connection.bitfieldSent == expected.bitfieldSent);
        check(connection.outgoingRequests == expected.outgoingRequests);
        check(connection.acceptedRequests == expected.acceptedRequests);
        check(connection.scheduledRequests == expected.scheduledRequests);
        check(connection.weAreInterestedInRemote == expected.weAreInterestedInRemote);
        check(connection.handshakeComplete() == expected.handshakeComplete());
        check(connection.remoteIsChokingUs == expected.remoteIsChokingUs);
        check(connection.weAreChokingRemote == expected.weAreChokingRemote);
        check(connection.remoteInterestedInUs == expected.remoteInterestedInUs);
        check(connection.remoteBitfield == expected.remoteBitfield);
    }
}

void unchoke()
{
    Fixture f;
    f.receive(Message(MessageType::Choke));
    check(f.remote().remoteIsChokingUs);
    f.receive(Message(MessageType::Unchoke));
    check(!f.remote().remoteIsChokingUs);
}

void choke()
{
    Fixture f;
    f.receive(Message(MessageType::Unchoke));
    check(!f.remote().remoteIsChokingUs);
    f.receive(Message(MessageType::Choke));
    check(f.remote().remoteIsChokingUs);
}

void interested()
{
    Fixture f;
    f.receive(Message(MessageType::Choke));
    check(!f.remote().remoteInterestedInUs);
    f.receive(Message(MessageType::Interested));
    check(f.remote().remoteInterestedInUs);
}

void notInterested()
{
    Fixture f;
    f.receive(Message(MessageType::Interested));
    check(f.remote().remoteInterestedInUs);
    f.receive(Message(MessageType::NotInterested));
    check(!f.remote().remoteInterestedInUs);
}

void have()
{
    Fixture f;
    f.receive(Message(MessageType::Have, HavePayload{7}));
    check(f.remote().remoteBitfield == std::vector<std::uint8_t>({0x01, 0x00}));
    f.receive(Message(MessageType::Have, HavePayload{8}));
    check(f.remote().remoteBitfield == std::vector<std::uint8_t>({0x01, 0x80}));
    f.receive(Message(MessageType::Have, HavePayload{0}));
    check(f.remote().remoteBitfield == std::vector<std::uint8_t>({0x81, 0x80}));
    f.receive(Message(MessageType::Have, HavePayload{8}));
    check(f.remote().remoteBitfield == std::vector<std::uint8_t>({0x81, 0x80}));
}

void rejectedWithoutMutation(Fixture& f, const Message& message)
{
    const auto before = f.receiver.swarmState(f.swarm.id());
    rejects([&] { f.receive(message); });
    checkUnchanged(f.receiver.swarmState(f.swarm.id()), before);
    // Invalid input must not create a connection for a previously unseen sender.
    rejects([&] { f.receive(message, 99); });
    checkUnchanged(f.receiver.swarmState(f.swarm.id()), before);
}

void invalidHave()
{
    Fixture f;
    f.seed();
    for (auto index : {9u, 10u, 0xffffffffu}) {
        rejectedWithoutMutation(f, Message(MessageType::Have, HavePayload{index}));
    }
}

void bitfield()
{
    Fixture f;
    f.seed();
    for (const auto& bytes : {std::vector<std::uint8_t>{0x5a, 0x80},
                             std::vector<std::uint8_t>{0, 0}}) {
        f.receive(Message(MessageType::Bitfield, BitfieldPayload{bytes}));
        check(f.remote().remoteBitfield == bytes);
        check(!f.remote().remoteIsChokingUs && f.remote().remoteInterestedInUs);
    }
    const Swarm aligned(2, {}, 8);
    f.receiver.joinSwarm(aligned);
    establishHandshake(f.receiver, aligned);
    f.receiver.receiveMessage(aligned, 1, Message(MessageType::Bitfield, BitfieldPayload{{0xff}}));
    check(f.receiver.swarmState(2).connections.at(1).remoteBitfield
          == std::vector<std::uint8_t>({0xff}));
}

void invalidBitfieldLength()
{
    Fixture f;
    f.seed();
    for (const auto& bytes : {std::vector<std::uint8_t>{},
                             std::vector<std::uint8_t>{0x80},
                             std::vector<std::uint8_t>{0x80, 0x80, 0}}) {
        rejectedWithoutMutation(f, Message(MessageType::Bitfield, BitfieldPayload{bytes}));
    }
}

void invalidBitfieldTrailingBits()
{
    Fixture f;
    f.seed();
    // Nine pieces leave seven unused low bits in the second byte.
    for (unsigned bit = 0; bit < 7; ++bit) {
        rejectedWithoutMutation(f, Message(MessageType::Bitfield,
            BitfieldPayload{{0x5a, static_cast<std::uint8_t>(0x80u | (1u << bit))}}));
    }
}

void unjoinedSwarm()
{
    Fixture f;
    f.seed();
    const Swarm unjoined(2, {}, 9);
    const auto before = f.receiver.swarmState(1);
    for (const auto& message : {
            Message(MessageType::Unchoke), Message(MessageType::Choke),
            Message(MessageType::Interested), Message(MessageType::NotInterested),
            Message(MessageType::Have, HavePayload{8}),
            Message(MessageType::Bitfield, BitfieldPayload{{0x80, 0x80}})}) {
        rejects([&] { f.receiver.receiveMessage(unjoined, 1, message); });
        check(!f.receiver.hasSwarm(unjoined.id()));
        checkUnchanged(f.receiver.swarmState(1), before);
    }
}

class ObserveChokingEvent : public Event {
public:
    ObserveChokingEvent(double time, const Network& network, bool expected, int& observations)
        : Event(time), network_(network), expected_(expected), observations_(observations) {}
    void execute() override {
        check(network_.peer(2).swarmState(1).connections.at(1).remoteIsChokingUs == expected_);
        ++observations_;
    }
private:
    const Network& network_;
    bool expected_;
    int& observations_;
};

void arrivalTiming()
{
    Fixture f;
    f.receive(Message(MessageType::Choke));
    Peer sender(1, 1000, 1000, testSenderProtocolId);
    sender.joinSwarm(f.swarm);
    Simulation simulation;
    Network network(simulation, {sender, f.receiver}, {Link(1, 2, 800, 0.1)}, {f.swarm});
    int observations = 0;
    simulation.schedule(std::make_unique<SendMessageEvent>(
        2.0, network, 1, 1, 2, Message(MessageType::Unchoke)));
    // FIFO at equal timestamps places this observer after SendMessageEvent.
    simulation.schedule(std::make_unique<ObserveChokingEvent>(2.0, network, true, observations));
    // Arrival is 2 + 0.1 latency + 5 * 8 / 500 transmission = 2.18.
    simulation.schedule(std::make_unique<ObserveChokingEvent>(2.179, network, true, observations));
    simulation.schedule(std::make_unique<ObserveChokingEvent>(2.181, network, false, observations));
    check(network.peer(2).swarmState(1).connections.at(1).remoteIsChokingUs);
    simulation.run();
    check(observations == 3);
    check(!network.peer(2).swarmState(1).connections.at(1).remoteIsChokingUs);
}
struct HandshakeFixture {
    Swarm swarm{1, InfoHash{0x13, 0x57}, 9};
    Swarm other{2, InfoHash{0x24}, 9};
    Peer sender{1, 1000, 1000, testSenderProtocolId};
    Peer receiver{2, 500, 500, PeerProtocolId{0xb2}};
    Peer third{3, 1000, 1000, PeerProtocolId{0xc3}};
    Simulation simulation;
    Network network;

    HandshakeFixture()
        : network(simulation, joinedPeers(), {Link(1, 2, 800, 0.1), Link(2, 3, 800, 0.1)}, {swarm, other}) {}
    std::vector<Peer> joinedPeers() {
        for (Peer* peer : {&sender, &receiver, &third}) {
            peer->joinSwarm(swarm);
            peer->joinSwarm(other);
        }
        return {sender, receiver, third};
    }
    void deliver(const Message& message) { network.deliver(1, 1, 2, message); }
    const PeerSwarmState& state() const { return network.peer(2).swarmState(1); }
    void complete() {
        network.send(1, 1, 2, handshake(swarm, sender.protocolId()));
        simulation.run();
    }
};

class CheckEvent : public Event {
public:
    CheckEvent(double time, std::function<void()> action)
        : Event(time), action_(std::move(action)) {}
    void execute() override { action_(); }
private:
    std::function<void()> action_;
};

struct QueueFixture {
    Swarm swarm{1, InfoHash{0x42}, 8};
    Peer a{1, 1000, 1000, PeerProtocolId{0xa1}};
    Peer b{2, 1000, 1000, PeerProtocolId{0xb2}};
    Simulation simulation;
    Network network;

    explicit QueueFixture(double latency = 1.0, double bandwidth = 800)
        : network(simulation, joinedPeers(), {Link(1, 2, bandwidth, latency)}, {swarm}) {}
    std::vector<Peer> joinedPeers() {
        // Transport-only fixture: all pieces owned, so availability adds no interest traffic.
        a.joinSwarm(swarm, {0xff});
        b.joinSwarm(swarm, {0xff});
        a.markHandshakeSent(swarm, 2);
        b.markHandshakeSent(swarm, 1);
        a.receiveMessage(swarm, 2, handshake(swarm, b.protocolId()), &b.protocolId());
        b.receiveMessage(swarm, 1, handshake(swarm, a.protocolId()), &a.protocolId());
        return {a, b};
    }
    void send(double time, PeerId sender, Message message) {
        simulation.schedule(std::make_unique<SendMessageEvent>(
            time, network, 1, sender, sender == 1 ? 2 : 1, std::move(message)));
    }
    void observe(double time, std::function<void()> action) {
        simulation.schedule(std::make_unique<CheckEvent>(time, std::move(action)));
    }
    const PeerConnectionState& remote(PeerId receiver = 2) const {
        return network.peer(receiver).swarmState(1).connections.at(receiver == 1 ? 2 : 1);
    }
};

// A valid 5,000,000-byte BITFIELD wire message gives exactly 40,000,000 bits.
// All peers own every piece, so its delivery cannot generate interest traffic.
struct MbpsFixture {
    static constexpr std::size_t payloadBytes = 4999995;
    Swarm swarm{1, InfoHash{0x42}, static_cast<std::uint32_t>(payloadBytes * 8)};
    Simulation simulation;
    Network network;

    std::vector<Peer> peers(bool independent) {
        std::vector<Peer> result;
        for (PeerId id = 1; id <= (independent ? 4u : 2u); ++id) {
            result.emplace_back(id, 100000000, 100000000);
            result.back().joinSwarm(swarm, std::vector<std::uint8_t>(payloadBytes, 0xff));
        }
        for (std::size_t i = 0; i < result.size(); i += 2) {
            auto& a = result[i];
            auto& b = result[i + 1];
            a.markHandshakeSent(swarm, b.id());
            b.markHandshakeSent(swarm, a.id());
            a.receiveMessage(swarm, b.id(), handshake(swarm, b.protocolId()), &b.protocolId());
            b.receiveMessage(swarm, a.id(), handshake(swarm, a.protocolId()), &a.protocolId());
        }
        return result;
    }
    explicit MbpsFixture(double rate, bool independent = false)
        : network(simulation, peers(independent), independent
            ? std::vector<Link>{Link(1, 2, rate, 0.25), Link(3, 4, rate, 0.25)}
            : std::vector<Link>{Link(1, 2, rate, 0.25)}, {swarm}) {}
    TransmissionId start(PeerId sender = 1, PeerId receiver = 2) {
        Message message(MessageType::Bitfield, BitfieldPayload{std::vector<std::uint8_t>(payloadBytes, 0)});
        check(message.wireSize() * 8 == 40000000);
        network.send(1, sender, receiver, std::move(message));
        for (const auto& [id, active] : network.activeTransmissions()) {
            if (active.sender == sender && active.receiver == receiver) return id;
        }
        throw std::runtime_error("Expected active Mbps transmission");
    }
    void at(double time, std::function<void()> action) {
        simulation.schedule(std::make_unique<CheckEvent>(time, std::move(action)));
    }
};

void checkRateRecord(const ActiveTransmission& actual, const ActiveTransmission& expected)
{
    check(actual.id == expected.id && actual.sender == expected.sender && actual.receiver == expected.receiver);
    check(actual.swarmId == expected.swarmId && actual.linkIndex == expected.linkIndex);
    check(actual.remainingBits == expected.remainingBits && actual.currentRate == expected.currentRate);
    check(actual.lastRateUpdateTime == expected.lastRateUpdateTime && actual.generation == expected.generation);
    check(actual.message.type() == expected.message.type() && actual.message.wireSize() == expected.message.wireSize());
}

struct RateExpectation { double time, rate, remaining; };

void exerciseMbpsRates(double initialRate, std::vector<RateExpectation> changes,
                      double completion, bool auditStale = false)
{
    MbpsFixture f(initialRate);
    const auto id = f.start();
    check(f.network.activeTransmissions().at(id).currentRate == initialRate);
    f.network.send(1, 1, 2, Message(MessageType::Unchoke));
    unsigned stale = 0, valid = 0, firstArrivals = 0, secondStarts = 0, secondArrivals = 0;
    unsigned completionEffects = 0, staleAudits = 0;
    std::vector<double> predictions{40000000.0 / initialRate};
    for (const auto& change : changes) predictions.push_back(change.time + change.remaining / change.rate);
    f.simulation.setEventObserver([&](const Event& event) {
        if (const auto* tx = dynamic_cast<const TransmissionCompleteEvent*>(&event);
            tx && tx->transmissionId() == id) {
            check(std::abs(event.time() - predictions.at(tx->generation())) < 1e-12);
            if (tx->generation() == changes.size()) {
                ++valid;
                check(std::abs(event.time() - completion) < 1e-12);
                check(f.network.activeTransmissions().contains(id));
                check(f.network.transmissionState(1, 2).pendingCount == 1);
                f.at(event.time(), [&] {
                    check(!f.network.activeTransmissions().contains(id));
                    check(secondStarts == 1);
                    ++completionEffects;
                });
            } else {
                ++stale;
                if (auditStale) {
                    // Audit obsolete completions both during and after the active lifetime,
                    // away from other event timestamps. Snapshot immediately around execution.
                    const auto active = f.network.activeTransmissions();
                    const auto direction = f.network.transmissionState(1, 2);
                    const auto a = f.network.peer(1).swarmState(1);
                    const auto b = f.network.peer(2).swarmState(1);
                    const auto startsBefore = secondStarts;
                    const auto arrivalsBefore = firstArrivals + secondArrivals;
                    f.at(event.time(), [&, active, direction, a, b, startsBefore, arrivalsBefore] {
                        check(f.network.activeTransmissions().size() == active.size());
                        for (const auto& [activeId, before] : active) {
                            checkRateRecord(f.network.activeTransmissions().at(activeId), before);
                        }
                        check(f.network.transmissionState(1, 2).active == direction.active);
                        check(f.network.transmissionState(1, 2).pendingCount == direction.pendingCount);
                        check(secondStarts == startsBefore && firstArrivals + secondArrivals == arrivalsBefore);
                        checkUnchanged(f.network.peer(1).swarmState(1), a);
                        checkUnchanged(f.network.peer(2).swarmState(1), b);
                        ++staleAudits;
                    });
                }
            }
        }
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
            start && start->message().type() == MessageType::Unchoke) {
            ++secondStarts;
            check(valid == 1 && !f.network.activeTransmissions().contains(id));
            check(std::abs(event.time() - completion) < 1e-12);
        }
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event)) {
            if (arrival->details().messageType == MessageType::Bitfield) {
                ++firstArrivals;
                check(valid == 1 && std::abs(event.time() - (completion + 0.25)) < 1e-12);
            } else {
                check(arrival->details().messageType == MessageType::Unchoke);
                ++secondArrivals;
                check(std::abs(event.time() - (completion + 40 / initialRate + 0.25)) < 1e-12);
            }
        }
    });
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const auto change = changes[i];
        f.at(change.time, [&, change, i] {
            f.network.setTransmissionRate(id, change.rate);
            const auto& active = f.network.activeTransmissions().at(id);
            check(active.remainingBits >= 0);
            check(std::abs(active.remainingBits - change.remaining) < 1e-6);
            check(active.currentRate == change.rate && active.lastRateUpdateTime == change.time);
            check(active.generation == i + 1);
            check(f.network.transmissionState(1, 2).active && f.network.transmissionState(1, 2).pendingCount == 1);
        });
    }
    f.simulation.run();
    check(stale == changes.size() && valid == 1 && completionEffects == 1);
    check(firstArrivals == 1 && secondStarts == 1 && secondArrivals == 1);
    check(!auditStale || staleAudits == stale);
    check(f.network.activeTransmissions().empty() && !f.network.transmissionState(1, 2).active);
    check(!f.network.peer(2).swarmState(1).connections.at(1).remoteIsChokingUs);
}

void mbpsSlowdown() { exerciseMbpsRates(10000000, {{2, 5000000, 20000000}}, 6, true); }
void mbpsSpeedup() { exerciseMbpsRates(5000000, {{2, 10000000, 30000000}}, 5); }
void mbpsMultipleChanges() {
    // First 2 s transfer 20 Mbit; next 1 s transfers 5 Mbit; final 15 Mbit takes .75 s.
    exerciseMbpsRates(10000000, {{2, 5000000, 20000000}, {3, 20000000, 15000000}}, 3.75);
}
void mbpsSameStartTime() { exerciseMbpsRates(10000000, {{0, 5000000, 40000000}}, 8, true); }
void mbpsNearCompletion() {
    const double time = std::nextafter(4.0, 0.0);
    // Independent higher-precision interval calculation; less than one microbit remains.
    const double remaining = static_cast<double>((4.0L - static_cast<long double>(time)) * 10000000.0L);
    exerciseMbpsRates(10000000, {{time, 5000000, remaining}}, time + remaining / 5000000);
}
void mbpsStaleSafety() {
    // Obsolete predictions t=4,7,12 all execute while generation 3 still owns the FIFO.
    exerciseMbpsRates(10000000,
        {{1, 5000000, 30000000}, {2, 2500000, 25000000}, {3, 1250000, 22500000}}, 21, true);
}
void mbpsFifo() {
    exerciseMbpsRates(10000000,
        {{1, 5000000, 30000000}, {2, 10000000, 25000000}, {3, 5000000, 15000000}}, 6, true);
}
void mbpsUniqueness() {
    // Several completions become stale even when overrides share the same timestamp.
    exerciseMbpsRates(10000000,
        {{2, 5000000, 20000000}, {2, 10000000, 20000000}, {2, 20000000, 20000000}}, 3);
}

void mbpsUnaffected(bool independent)
{
    std::vector<std::pair<int, double>> baseline;
    for (bool overrideRate : {false, true}) {
        MbpsFixture f(10000000, independent);
        const auto first = f.start();
        const PeerId sender = independent ? 3 : 2, receiver = independent ? 4 : 1;
        const auto other = f.start(sender, receiver);
        const auto original = f.network.activeTransmissions().at(other);
        f.network.send(1, sender, receiver, Message(MessageType::Unchoke));
        std::vector<std::pair<int, double>> trace;
        unsigned starts = 0, completions = 0, arrivals = 0;
        f.simulation.setEventObserver([&](const Event& event) {
            if (const auto* tx = dynamic_cast<const TransmissionCompleteEvent*>(&event);
                tx && tx->details() && tx->details()->sender == sender) {
                ++completions;
                trace.emplace_back(0, event.time());
                check(tx->generation() == 0);
                if (tx->transmissionId() == other) {
                    check(event.time() == 4);
                    checkRateRecord(f.network.activeTransmissions().at(other), original);
                } else check(std::abs(event.time() - 4.000004) < 1e-12);
            }
            if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
                start && start->details().sender == sender) {
                ++starts;
                trace.emplace_back(1, event.time());
                check(event.time() == 4 && completions == 1);
            }
            if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event);
                arrival && arrival->details().sender == sender) {
                ++arrivals;
                trace.emplace_back(2, event.time());
            }
        });
        f.at(2, [&] {
            if (overrideRate) f.network.setTransmissionRate(first, 5000000);
            const auto& active = f.network.activeTransmissions().at(other);
            checkRateRecord(active, original);
            // remainingBits is anchored to lastRateUpdateTime; derived progress is 20 Mbit.
            check(active.remainingBits - active.currentRate * (f.simulation.currentTime() - active.lastRateUpdateTime) == 20000000);
            check(f.network.transmissionState(sender, receiver).active);
            check(f.network.transmissionState(sender, receiver).pendingCount == 1);
        });
        f.simulation.run();
        check(starts == 1 && completions == 2 && arrivals == 2);
        check(!f.network.transmissionState(sender, receiver).active);
        check(!f.network.peer(receiver).swarmState(1).connections.at(sender).remoteIsChokingUs);
        if (!overrideRate) baseline = trace;
        else check(trace == baseline);
    }
}
void mbpsFullDuplex() { mbpsUnaffected(false); }
void mbpsIndependentLink() { mbpsUnaffected(true); }

struct SharingFixture {
    Swarm swarm{1, InfoHash{0x42}, 8};
    Simulation simulation;
    Network network;
    std::vector<Peer> peers(const std::vector<double>& upload, const std::vector<double>& download) {
        std::vector<Peer> result;
        for (PeerId id = 1; id <= 4; ++id) {
            result.emplace_back(id, upload[id - 1], download[id - 1]);
            result.back().joinSwarm(swarm, {0xff});
        }
        for (auto& a : result) for (auto& b : result) if (a.id() != b.id()) {
            a.markHandshakeSent(swarm, b.id());
            a.receiveMessage(swarm, b.id(), handshake(swarm, b.protocolId()), &b.protocolId());
        }
        return result;
    }
    explicit SharingFixture(std::vector<double> upload = {1e7, 1e8, 1e8, 1e8},
                            std::vector<double> download = {1e8, 1e8, 1e8, 1e8},
                            double link12 = 1e8)
        : network(simulation, peers(upload, download),
            {Link(1, 2, link12, .25), Link(1, 3, 1e8, .25), Link(1, 4, 1e8, .25),
             Link(2, 3, 1e8, .25), Link(2, 4, 1e8, .25), Link(3, 4, 1e8, .25)}, {swarm}) {}
    TransmissionId send(PeerId sender, PeerId receiver, bool shortMessage = false) {
        network.send(1, sender, receiver, shortMessage ? Message(MessageType::Unchoke)
            : Message(MessageType::Cancel, CancelPayload{}));
        invariant();
        for (const auto& [id, active] : network.activeTransmissions()) {
            if (active.sender == sender && active.receiver == receiver) return id;
        }
        throw std::runtime_error("Missing active sharing transmission");
    }
    const ActiveTransmission& active(TransmissionId id) const { return network.activeTransmissions().at(id); }
    void at(double time, std::function<void()> action) {
        simulation.schedule(std::make_unique<CheckEvent>(time, std::move(action)));
    }
    void invariant() const {
        std::map<PeerId, double> outgoing, incoming;
        std::set<std::pair<PeerId, PeerId>> directions;
        for (const auto& [id, active] : network.activeTransmissions()) {
            check(active.remainingBits >= 0 && std::isfinite(active.currentRate) && active.currentRate > 0);
            check(directions.emplace(active.sender, active.receiver).second);
            outgoing[active.sender] += active.currentRate;
            incoming[active.receiver] += active.currentRate;
        }
        for (PeerId id = 1; id <= 4; ++id) {
            check(outgoing[id] <= network.peer(id).uploadCapacity() * (1 + 1e-14));
            check(incoming[id] <= network.peer(id).downloadCapacity() * (1 + 1e-14));
        }
    }
};

void equalShareExplicitSixFourFour()
{
    std::vector<std::pair<TransmissionId, double>> baseline;
    for (int run = 0; run < 2; ++run) {
        SharingFixture f({12e6, 1e6, 100e6, 20e6}, {100e6, 20e6, 8e6, 100e6});
        // P2 -> P4 uses upload(P2)/download(P4), neither of the contested budgets.
        // At 1 Mbps it remains active throughout all three relevant completions.
        const auto unrelated = f.send(2, 4);
        const auto unchanged = f.active(unrelated);
        const auto toTwo = f.send(1, 2);       // CANCEL: 136 wire bits.
        const auto toThree = f.send(1, 3, true); // UNCHOKE: 40 wire bits; finishes first.
        const auto fromFour = f.send(4, 3);    // CANCEL: 136 wire bits.
        const auto beforeTwo = f.active(toTwo), beforeFour = f.active(fromFour);
        check(beforeTwo.currentRate == 6e6);
        check(f.active(toThree).currentRate == 4e6 && beforeFour.currentRate == 4e6);
        const double outgoingOne = beforeTwo.currentRate + f.active(toThree).currentRate;
        check(outgoingOne == 10e6 && outgoingOne <= f.network.peer(1).uploadCapacity());
        check(f.active(toThree).currentRate + beforeFour.currentRate == 8e6);
        checkRateRecord(f.active(unrelated), unchanged);

        // At t=10 us, P1 -> P3 finishes 40 bits at 4 Mbps.
        // P1 -> P2 has sent 60 bits at 6 Mbps: 76 remain, now at 12 Mbps.
        // P4 -> P3 has sent 40 bits at 4 Mbps: 96 remain, now at 8 Mbps.
        const double firstCompletion = 10e-6;
        const double twoCompletion = firstCompletion + 76.0 / 12e6;
        const double fourCompletion = firstCompletion + 96.0 / 8e6;
        std::map<TransmissionId, double> expected{
            {toThree, firstCompletion}, {toTwo, twoCompletion},
            {fromFour, fourCompletion}, {unrelated, 136.0 / 1e6}};
        std::map<TransmissionId, unsigned> valid, stale;
        std::map<std::pair<PeerId, PeerId>, unsigned> arrivals;
        std::vector<std::pair<TransmissionId, double>> trace;
        f.simulation.setEventObserver([&](const Event& event) {
            f.invariant();
            if (f.network.activeTransmissions().contains(unrelated)) {
                checkRateRecord(f.active(unrelated), unchanged);
            }
            if (const auto* tx = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
                const auto found = f.network.activeTransmissions().find(tx->transmissionId());
                if (found == f.network.activeTransmissions().end() || found->second.generation != tx->generation()) {
                    ++stale[tx->transmissionId()];
                } else {
                    ++valid[tx->transmissionId()];
                    check(std::abs(event.time() - expected.at(tx->transmissionId())) < 1e-15);
                    trace.emplace_back(tx->transmissionId(), event.time());
                }
            }
            if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event)) {
                const auto details = arrival->details();
                ++arrivals[{details.sender, details.receiver}];
                const auto id = details.sender == 2 ? unrelated : details.sender == 4 ? fromFour
                    : details.receiver == 2 ? toTwo : toThree;
                check(valid[id] == 1);
                check(std::abs(event.time() - (expected.at(id) + .25)) < 1e-15);
            }
        });
        f.at(firstCompletion, [&] {
            check(valid[toThree] == 1 && !f.network.activeTransmissions().contains(toThree));
            const auto& two = f.active(toTwo);
            const auto& four = f.active(fromFour);
            check(std::abs(two.remainingBits - 76) < 1e-12 && two.currentRate == 12e6);
            check(std::abs(four.remainingBits - 96) < 1e-12 && four.currentRate == 8e6);
            check(two.lastRateUpdateTime == firstCompletion && four.lastRateUpdateTime == firstCompletion);
            check(two.generation == beforeTwo.generation + 1 && four.generation == beforeFour.generation + 1);
            checkRateRecord(f.active(unrelated), unchanged);
        });
        f.simulation.run();
        for (const auto& [id, time] : expected) check(valid[id] == 1);
        check(stale[toTwo] >= 1 && stale[fromFour] == 1 && stale[unrelated] == 0);
        check(arrivals.size() == 4);
        for (const auto& [direction, count] : arrivals) check(count == 1);
        check(f.network.activeTransmissions().empty());
        if (run == 0) baseline = trace;
        else check(trace == baseline);
    }
}

void equalShareUpload()
{
    SharingFixture f;
    const auto a = f.send(1, 2), b = f.send(1, 3);
    check(f.active(a).currentRate == 5e6 && f.active(b).currentRate == 5e6);
    check(f.active(a).remainingBits == 136 && f.active(b).remainingBits == 136);
    f.simulation.run();
    check(f.network.activeTransmissions().empty());
}

void equalShareUploadRelease()
{
    SharingFixture f;
    const auto shortId = f.send(1, 2, true), longId = f.send(1, 3);
    unsigned valid = 0, stale = 0, arrivals = 0;
    f.simulation.setEventObserver([&](const Event& event) {
        f.invariant();
        if (const auto* tx = dynamic_cast<const TransmissionCompleteEvent*>(&event);
            tx && tx->transmissionId() == longId) {
            const auto found = f.network.activeTransmissions().find(longId);
            if (found == f.network.activeTransmissions().end() || found->second.generation != tx->generation()) {
                ++stale;
                check(std::abs(event.time() - 27.2e-6) < 1e-15);
            } else {
                ++valid;
                check(std::abs(event.time() - 17.6e-6) < 1e-15);
            }
        }
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event);
            arrival && arrival->details().receiver == 3) {
            ++arrivals;
            check(std::abs(event.time() - (.25 + 17.6e-6)) < 1e-15);
        }
    });
    f.at(9e-6, [&] {
        check(!f.network.activeTransmissions().contains(shortId));
        const auto& active = f.active(longId);
        // 5 Mbps * 8 microseconds = 40 bits sent; 96 bits remain.
        check(std::abs(active.remainingBits - 96) < 1e-12);
        check(active.currentRate == 1e7 && active.generation == 1);
        check(std::abs(active.lastRateUpdateTime - 8e-6) < 1e-15);
    });
    f.simulation.run();
    check(valid == 1 && stale == 1 && arrivals == 1);
}

void equalShareDownload()
{
    SharingFixture f({1e8, 1e8, 1e8, 1e8}, {1e8, 1e8, 1e7, 1e8});
    const auto a = f.send(1, 3), b = f.send(2, 3);
    check(f.active(a).currentRate == 5e6 && f.active(b).currentRate == 5e6);
    f.simulation.run();
}

void equalShareEndpointLimits()
{
    SharingFixture f({12e6, 1e8, 1e8, 1e8}, {1e8, 1e8, 1e7, 1e8}, 2e6);
    const auto a = f.send(1, 3), b = f.send(1, 2), c = f.send(4, 3);
    check(f.active(a).currentRate == 5e6); // min(6 Mbps upload, 5 Mbps download, 100 Mbps link).
    check(f.active(b).currentRate == 2e6 && f.active(c).currentRate == 5e6);
    f.simulation.run();
}

void equalShareLinkCapUnchanged()
{
    SharingFixture f({1e7, 1e8, 1e8, 1e8}, {1e8, 1e8, 1e8, 1e8}, 1e6);
    const auto a = f.send(1, 2);
    f.at(2e-6, [&] {
        const auto b = f.send(1, 3);
        check(f.active(a).currentRate == 1e6 && f.active(a).generation == 0);
        check(f.active(a).remainingBits == 134); // Progress updated, no redundant completion scheduled.
        check(f.active(b).currentRate == 5e6); // Unused share is not redistributed.
    });
    unsigned completions = 0;
    f.simulation.setEventObserver([&](const Event& event) {
        if (const auto* tx = dynamic_cast<const TransmissionCompleteEvent*>(&event);
            tx && tx->transmissionId() == a) {
            ++completions;
            check(tx->generation() == 0 && std::abs(event.time() - 136e-6) < 1e-15);
        }
    });
    f.simulation.run();
    check(completions == 1);
}

void equalShareUnrelated()
{
    SharingFixture isolated;
    const auto id = isolated.send(3, 4);
    const auto before = isolated.active(id);
    isolated.send(1, 2, true);
    isolated.send(2, 1, true);
    checkRateRecord(isolated.active(id), before);
    unsigned completions = 0;
    isolated.simulation.setEventObserver([&](const Event& event) {
        if (const auto* tx = dynamic_cast<const TransmissionCompleteEvent*>(&event);
            tx && tx->transmissionId() == id) {
            ++completions;
            checkRateRecord(isolated.active(id), before);
            check(std::abs(event.time() - 1.36e-6) < 1e-15 && tx->generation() == 0);
        }
    });
    isolated.simulation.run();
    check(completions == 1);
}

void equalShareFifo()
{
    SharingFixture f;
    f.send(1, 2, true);
    const auto b = f.send(1, 3);
    f.send(1, 2); // FIFO entry, not a third flow.
    check(f.network.activeTransmissions().size() == 2 && f.active(b).currentRate == 5e6);
    check(f.network.transmissionState(1, 2).pendingCount == 1);
    unsigned starts = 0;
    f.simulation.setEventObserver([&](const Event& event) {
        f.invariant();
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
            start && start->details().receiver == 2) {
            ++starts;
            check(std::abs(event.time() - 8e-6) < 1e-15);
        }
    });
    f.at(9e-6, [&] {
        check(starts == 1 && f.network.transmissionState(1, 2).pendingCount == 0);
        check(f.active(b).currentRate == 5e6 && std::abs(f.active(b).remainingBits - 96) < 1e-12);
    });
    f.simulation.run();
    check(starts == 1);
}

void equalShareDuplex()
{
    SharingFixture f({1e7, 8e6, 1e8, 1e8}, {6e6, 2e7, 1e8, 1e8});
    const auto a = f.send(1, 2), b = f.send(2, 1);
    const auto reverse = f.active(b);
    check(f.active(a).currentRate == 1e7 && reverse.currentRate == 6e6);
    f.send(1, 3);
    check(f.active(a).currentRate == 5e6);
    checkRateRecord(f.active(b), reverse);
    f.simulation.run();
}

void equalShareSameTimestamp()
{
    std::vector<std::pair<TransmissionId, double>> baseline;
    for (int run = 0; run < 2; ++run) {
        SharingFixture f;
        for (PeerId receiver : {2u, 3u, 4u}) {
            f.simulation.schedule(std::make_unique<SendMessageEvent>(0, f.network, 1, 1, receiver,
                Message(MessageType::Cancel, CancelPayload{})));
        }
        f.at(0, [&] {
            check(f.network.activeTransmissions().size() == 3);
            for (const auto& [id, active] : f.network.activeTransmissions()) {
                check(active.remainingBits == 136 && active.lastRateUpdateTime == 0);
                check(active.currentRate == 1e7 / 3);
            }
        });
        std::vector<std::pair<TransmissionId, double>> trace;
        f.simulation.setEventObserver([&](const Event& event) {
            f.invariant();
            if (const auto* tx = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
                const auto found = f.network.activeTransmissions().find(tx->transmissionId());
                if (found != f.network.activeTransmissions().end() && found->second.generation == tx->generation()) {
                    check(std::abs(event.time() - 40.8e-6) < 1e-15);
                    trace.emplace_back(tx->transmissionId(), event.time());
                }
            }
        });
        f.simulation.run();
        check(trace.size() == 3);
        if (run == 0) baseline = trace;
        else check(trace == baseline);
    }
}

void equalShareAggregateTransitions()
{
    SharingFixture f({1e7, 12e6, 8e6, 16e6}, {6e6, 14e6, 1e7, 9e6}, 2e6);
    unsigned observations = 0;
    f.simulation.setEventObserver([&](const Event& event) {
        f.invariant();
        ++observations;
        if (!dynamic_cast<const CheckEvent*>(&event)) f.at(event.time(), [&] { f.invariant(); });
    });
    for (PeerId sender = 1; sender <= 4; ++sender) for (PeerId receiver = 1; receiver <= 4; ++receiver) {
        if (sender == receiver) continue;
        f.simulation.schedule(std::make_unique<SendMessageEvent>((sender - 1) * 1e-6,
            f.network, 1, sender, receiver, Message(MessageType::Cancel, CancelPayload{})));
        f.simulation.schedule(std::make_unique<SendMessageEvent>((sender - 1) * 1e-6,
            f.network, 1, sender, receiver, Message(MessageType::Unchoke)));
    }
    f.simulation.run();
    check(observations > 100 && f.network.activeTransmissions().empty());
}

void equalShareStartProgress()
{
    SharingFixture f;
    const auto a = f.send(1, 2);
    f.at(2e-6, [&] {
        const auto b = f.send(1, 3);
        check(f.active(a).remainingBits == 116 && f.active(a).lastRateUpdateTime == 2e-6);
        check(f.active(a).currentRate == 5e6 && f.active(b).remainingBits == 136);
    });
    f.simulation.run();
}

void dynamicTransmissionRateMath()
{
    for (double newRate : {5.0, 20.0}) {
        QueueFixture f(0.5, 10);
        f.network.send(1, 1, 2, Message(MessageType::Unchoke)); // 40 bits: original completion t=4.
        const auto id = f.network.activeTransmissions().begin()->first;
        f.network.send(1, 1, 2, Message(MessageType::Choke));
        const double expected = 2.0 + 20.0 / newRate;
        unsigned stale = 0, valid = 0, arrivals = 0;
        f.simulation.setEventObserver([&](const Event& event) {
            if (const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event);
                complete && complete->transmissionId() == id) {
                if (complete->generation() == 0) {
                    ++stale;
                    check(event.time() == 4.0);
                    const auto before = f.network.activeTransmissions().begin()->second;
                    const auto pending = f.network.transmissionState(1, 2).pendingCount;
                    const auto peerBefore = f.network.peer(2).swarmState(1);
                    f.observe(event.time(), [&, before, pending, peerBefore] {
                        const auto& after = f.network.activeTransmissions().at(before.id);
                        check(after.generation == before.generation);
                        check(after.remainingBits == before.remainingBits && after.currentRate == before.currentRate);
                        check(after.lastRateUpdateTime == before.lastRateUpdateTime);
                        check(f.network.transmissionState(1, 2).active);
                        check(f.network.transmissionState(1, 2).pendingCount == pending);
                        checkUnchanged(f.network.peer(2).swarmState(1), peerBefore);
                    });
                } else {
                    ++valid;
                    check(complete->generation() == 1 && event.time() == expected);
                    check(f.network.activeTransmissions().contains(id));
                    check(f.network.transmissionState(1, 2).pendingCount == 1);
                }
            }
            if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
                start && start->message().type() == MessageType::Choke) {
                check(event.time() == expected && valid == 1);
                check(!f.network.activeTransmissions().contains(id));
            }
            if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event);
                arrival && arrival->details().messageType == MessageType::Unchoke) {
                ++arrivals;
                check(event.time() == expected + 0.5 && valid == 1);
            }
        });
        f.observe(2.0, [&] {
            f.network.setTransmissionRate(id, newRate);
            const auto& active = f.network.activeTransmissions().at(id);
            check(active.remainingBits == 20.0 && active.currentRate == newRate);
            check(active.lastRateUpdateTime == 2.0 && active.generation == 1);
            check(f.network.transmissionState(1, 2).pendingCount == 1);
        });
        f.simulation.run();
        check(stale == 1 && valid == 1 && arrivals == 1);
        check(f.network.activeTransmissions().empty() && f.remote().remoteIsChokingUs);
        // The following message still uses the original effective bandwidth (10).
        check(f.simulation.currentTime() == expected + 4.0 + 0.5);
    }
}

void repeatedTransmissionRateChanges()
{
    QueueFixture f(0.5, 10);
    f.network.send(1, 1, 2, Message(MessageType::Unchoke));
    const auto id = f.network.activeTransmissions().begin()->first;
    unsigned completions = 0, arrivals = 0;
    f.observe(2.0, [&] { f.network.setTransmissionRate(id, 5); });
    f.observe(3.0, [&] {
        f.network.setTransmissionRate(id, 20);
        const auto& active = f.network.activeTransmissions().at(id);
        check(active.remainingBits == 15 && active.lastRateUpdateTime == 3);
        check(active.generation == 2);
    });
    f.simulation.setEventObserver([&](const Event& event) {
        if (const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
            ++completions;
            if (complete->generation() == 2) check(event.time() == 3.75);
            else check(!f.network.activeTransmissions().contains(id));
        }
        if (dynamic_cast<const MessageArrivalEvent*>(&event)) {
            ++arrivals;
            check(event.time() == 4.25);
        }
    });
    f.simulation.run();
    check(completions == 3 && arrivals == 1);
    check(!f.remote().remoteIsChokingUs && f.network.activeTransmissions().empty());
}

void transmissionRateValidationAndBoundary()
{
    QueueFixture f(0.5, 10);
    TransmissionId id = 0;
    // Insert before the original completion so the override wins the timestamp tie.
    f.observe(4.0, [&] {
        f.network.setTransmissionRate(id, 7);
        const auto& active = f.network.activeTransmissions().at(id);
        check(active.remainingBits == 0 && active.generation == 1);
        check(active.lastRateUpdateTime == 4);
    });
    f.network.send(1, 1, 2, Message(MessageType::Unchoke));
    id = f.network.activeTransmissions().begin()->first;
    f.observe(2.0, [&] {
        for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::denorm_min()}) {
            rejects([&] { f.network.setTransmissionRate(id, rate); });
        }
        rejects([&] { f.network.setTransmissionRate(id + 100, 10); });
        const auto& active = f.network.activeTransmissions().at(id);
        check(active.remainingBits == 40 && active.currentRate == 10);
        check(active.lastRateUpdateTime == 0 && active.generation == 0);
    });
    unsigned arrivals = 0, completions = 0;
    f.simulation.setEventObserver([&](const Event& event) {
        if (dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
            check(event.time() == 4);
            ++completions;
        }
        if (dynamic_cast<const MessageArrivalEvent*>(&event)) {
            check(event.time() == 4.5);
            ++arrivals;
        }
    });
    f.simulation.run();
    check(arrivals == 1 && completions == 2);
    rejects([&] { f.network.setTransmissionRate(id, 10); });
}

void activeTransmissionLifecycle()
{
    QueueFixture f;
    check(f.network.activeTransmissions().empty());
    unsigned starts = 0;
    f.simulation.setEventObserver([&](const Event& event) {
        if (dynamic_cast<const TransmissionStartEvent*>(&event)) {
            // Observer runs before Start executes: the FIFO front is not active yet.
            check(!f.network.transmissionState(1, 2).active);
            check(f.network.activeTransmissions().empty());
            ++starts;
        }
    });
    f.network.send(1, 1, 2, Message(MessageType::Unchoke));
    const auto first = f.network.activeTransmissions().begin()->second;
    check(first.id != 0 && first.sender == 1 && first.receiver == 2 && first.swarmId == 1);
    check(first.message.type() == MessageType::Unchoke);
    check(first.remainingBits == 40 && first.currentRate == 800);
    check(first.lastRateUpdateTime == 0 && first.generation == 0);
    f.network.send(1, 1, 2, Message(MessageType::Have, HavePayload{7}));
    check(f.network.activeTransmissions().size() == 1);
    check(f.network.transmissionState(1, 2).pendingCount == 1);
    f.observe(0.051, [&] {
        check(f.network.activeTransmissions().size() == 1);
        const auto& second = f.network.activeTransmissions().begin()->second;
        check(second.id != first.id && second.message.type() == MessageType::Have);
        check(second.remainingBits == 72 && second.currentRate == 800);
        check(std::abs(second.lastRateUpdateTime - 0.05) < 1e-12);
        check(f.network.transmissionState(1, 2).pendingCount == 0);
    });
    f.observe(0.141, [&] {
        check(f.network.activeTransmissions().empty());
        check(!f.network.transmissionState(1, 2).active);
        check(f.remote().remoteIsChokingUs); // Still propagating.
    });
    f.simulation.run();
    check(starts == 2 && f.network.activeTransmissions().empty());
    check(!f.remote().remoteIsChokingUs);
}

void arrivalScheduledByCompletion()
{
    for (double latency : {0.0, 1.0}) {
        QueueFixture f(latency);
        bool markerRan = false;
        unsigned arrivals = 0, completions = 0;
        f.simulation.setEventObserver([&](const Event& event) {
            if (dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
                ++completions;
                check(f.network.activeTransmissions().size() == 1);
                // Enqueued immediately before completion executes. An arrival already
                // scheduled at Start would precede this marker at the same timestamp.
                f.observe(event.time() + latency, [&] { markerRan = true; });
            }
            if (dynamic_cast<const MessageArrivalEvent*>(&event)) {
                check(markerRan && completions == 1);
                check(f.network.activeTransmissions().empty());
                check(std::abs(event.time() - (0.05 + latency)) < 1e-12);
                ++arrivals;
            }
        });
        f.network.send(1, 1, 2, Message(MessageType::Unchoke));
        f.simulation.run();
        check(arrivals == 1 && !f.remote().remoteIsChokingUs);
    }
}

void staleCompletionNoOp()
{
    QueueFixture f;
    unsigned starts = 0, arrivals = 0;
    f.simulation.setEventObserver([&](const Event& event) {
        if (dynamic_cast<const TransmissionStartEvent*>(&event)) ++starts;
        if (dynamic_cast<const MessageArrivalEvent*>(&event)) ++arrivals;
    });
    f.network.send(1, 1, 2, Message(MessageType::Unchoke));
    f.network.send(1, 1, 2, Message(MessageType::Have, HavePayload{7}));
    const auto first = f.network.activeTransmissions().begin()->second;
    auto stale = first;
    ++stale.generation;
    f.simulation.schedule(std::make_unique<TransmissionCompleteEvent>(0.01, f.network, stale));
    // Unknown ID is also harmless.
    stale.id += 100;
    f.simulation.schedule(std::make_unique<TransmissionCompleteEvent>(0.02, f.network, stale));
    f.observe(0.03, [&] {
        const auto& actual = f.network.activeTransmissions().at(first.id);
        check(actual.remainingBits == first.remainingBits && actual.currentRate == first.currentRate);
        check(actual.lastRateUpdateTime == first.lastRateUpdateTime && actual.generation == first.generation);
        check(f.network.transmissionState(1, 2).active);
        check(f.network.transmissionState(1, 2).pendingCount == 1);
        check(starts == 1 && arrivals == 0);
        checkUnchanged(f.network.peer(1).swarmState(1), f.a.swarmState(1));
        checkUnchanged(f.network.peer(2).swarmState(1), f.b.swarmState(1));
    });
    // Old ID must not release the second message's direction. Exercise the
    // compatibility constructor too: it captures the first ID at construction.
    f.simulation.schedule(std::make_unique<TransmissionCompleteEvent>(
        0.06, f.network, first.linkIndex, first.sender));
    f.observe(0.07, [&] {
        check(f.network.activeTransmissions().size() == 1);
        check(f.network.activeTransmissions().begin()->first != first.id);
        check(f.network.transmissionState(1, 2).active && starts == 2 && arrivals == 0);
        checkUnchanged(f.network.peer(2).swarmState(1), f.b.swarmState(1));
    });
    f.simulation.schedule(std::make_unique<TransmissionCompleteEvent>(0.2, f.network, first));
    f.simulation.run();
    check(starts == 2 && arrivals == 2 && f.network.activeTransmissions().empty());
    check(!f.remote().remoteIsChokingUs && f.remote().remoteBitfield[0] == 1);
    check(std::abs(f.simulation.currentTime() - 1.14) < 1e-12);
}

void serializedTransmissionAndLatency()
{
    QueueFixture f;
    check(!f.network.transmissionState(1, 2).active);
    check(f.network.transmissionState(1, 2).pendingCount == 0);
    f.send(0, 1, Message(MessageType::Unchoke)); // 5 bytes: 0.05 seconds.
    f.send(0, 1, Message(MessageType::Have, HavePayload{7})); // 9 bytes: 0.09 seconds.
    f.observe(0.049, [&] {
        const auto state = f.network.transmissionState(1, 2);
        check(state.active && state.pendingCount == 1);
        check(f.remote().remoteIsChokingUs);
    });
    f.observe(0.051, [&] {
        const auto state = f.network.transmissionState(1, 2);
        check(state.active && state.pendingCount == 0); // Second started at 0.05.
        check(f.remote().remoteIsChokingUs); // First has not arrived.
    });
    f.observe(0.141, [&] {
        check(!f.network.transmissionState(1, 2).active);
        check(f.remote().remoteIsChokingUs);
        check(f.remote().remoteBitfield[0] == 0);
    });
    f.observe(1.049, [&] { check(f.remote().remoteIsChokingUs); });
    f.observe(1.051, [&] {
        check(!f.remote().remoteIsChokingUs);
        check(f.remote().remoteBitfield[0] == 0);
    });
    f.observe(1.139, [&] { check(f.remote().remoteBitfield[0] == 0); });
    f.simulation.run();
    check(f.remote().remoteBitfield[0] == 1);
    check(std::abs(f.simulation.currentTime() - 1.14) < 1e-12);
}

void fullDuplexTransmission()
{
    QueueFixture f;
    f.send(0, 1, Message(MessageType::Have, HavePayload{0}));
    f.send(0, 2, Message(MessageType::Have, HavePayload{7}));
    f.observe(0.04, [&] {
        check(f.network.transmissionState(1, 2).active);
        check(f.network.transmissionState(2, 1).active);
    });
    f.observe(0.091, [&] {
        check(!f.network.transmissionState(1, 2).active);
        check(!f.network.transmissionState(2, 1).active);
        check(f.remote(1).remoteBitfield[0] == 0);
        check(f.remote(2).remoteBitfield[0] == 0);
    });
    f.simulation.run();
    check(f.remote(1).remoteBitfield[0] == 1);
    check(f.remote(2).remoteBitfield[0] == 0x80);
    check(std::abs(f.simulation.currentTime() - 1.09) < 1e-12);
}

void fifoBurst()
{
    QueueFixture f;
    // One active message followed by thirty messages queued in the B -> A direction.
    for (unsigned i = 0; i < 31; ++i) {
        f.send(i == 0 ? 0 : 0.01, 2, Message(MessageType::Bitfield,
            BitfieldPayload{{static_cast<std::uint8_t>(i + 1)}}));
        f.observe(1.0 + (i + 1) * 0.06 + 0.001, [&, i] {
            check(f.remote(1).remoteBitfield[0] == i + 1);
        });
    }
    f.observe(0.02, [&] {
        const auto state = f.network.transmissionState(2, 1);
        check(state.active && state.pendingCount == 30);
        check(!f.network.transmissionState(1, 2).active);
    });
    f.observe(0.061, [&] {
        const auto state = f.network.transmissionState(2, 1);
        check(state.active && state.pendingCount == 29);
    });
    f.simulation.run();
    const auto state = f.network.transmissionState(2, 1);
    check(!state.active && state.pendingCount == 0);
    check(f.remote(1).remoteBitfield[0] == 31);
}

void enqueueAtCompletion()
{
    QueueFixture f(0);
    for (unsigned i = 0; i < 3; ++i) {
        // Third send executes at the first completion's timestamp.
        f.send(i == 2 ? 0.06 : 0, 1, Message(MessageType::Bitfield,
            BitfieldPayload{{static_cast<std::uint8_t>(i + 1)}}));
    }
    f.observe(0.061, [&] { check(f.remote().remoteBitfield[0] == 1); });
    f.observe(0.121, [&] { check(f.remote().remoteBitfield[0] == 2); });
    f.simulation.run();
    check(f.remote().remoteBitfield[0] == 3);
    check(!f.network.transmissionState(1, 2).active);
    check(f.network.transmissionState(1, 2).pendingCount == 0);
    check(std::abs(f.simulation.currentTime() - 0.18) < 1e-12);
}

void occupyDirection(Network& network, SwarmId swarm, PeerId sender, PeerId receiver)
{
    // Eight 17-byte CANCEL messages occupy 2.176 seconds at 500 bits/sec.
    for (int i = 0; i < 8; ++i) network.send(swarm, sender, receiver, Message(MessageType::Cancel, CancelPayload{}));
}

void queuedHandshakeResponse()
{
    HandshakeFixture f;
    // CANCEL traffic keeps the reverse direction busy.
    occupyDirection(f.network, 1, 2, 1);
    f.network.send(1, 1, 2, handshake(f.swarm));
    f.simulation.schedule(std::make_unique<CheckEvent>(1.189, [&] {
        const auto& b = f.state().connections.at(1);
        check(b.handshakeReceived && !b.handshakeSent && !b.handshakeComplete());
        check(f.network.transmissionState(2, 1).pendingCount == 4);
        f.deliver(handshake(f.swarm));
        f.deliver(handshake(f.swarm));
        check(f.network.transmissionState(2, 1).pendingCount == 4);
        const auto before = f.state();
        rejects([&] { f.deliver(Message(MessageType::Unchoke)); });
        checkUnchanged(f.state(), before);
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(2.177, [&] {
        check(f.state().connections.at(1).handshakeSent);
        check(f.state().connections.at(1).bitfieldSent);
        check(f.network.transmissionState(2, 1).pendingCount == 1); // Automatic BITFIELD.
        check(f.network.transmissionState(2, 1).active);
    }));
    f.simulation.run();
    check(f.state().connections.at(1).handshakeComplete());
    check(f.network.peer(1).swarmState(1).connections.at(2).handshakeComplete());
    check(!f.network.transmissionState(2, 1).active);
    check(std::abs(f.simulation.currentTime() - 3.576) < 1e-12);
}
struct AutoBitfieldFixture {
    Swarm swarm{1, InfoHash{0x13}, 9};
    Swarm other{2, InfoHash{0x24}, 8};
    Peer a{1, 1000, 1000, PeerProtocolId{0xa1}};
    Peer b{2, 500, 500, PeerProtocolId{0xb2}};
    Peer c{3, 1000, 1000, PeerProtocolId{0xc3}};
    Simulation simulation;
    Network network;

    AutoBitfieldFixture()
        : network(simulation, joinedPeers(),
            {Link(1, 2, 800, 0.1), Link(2, 3, 800, 0.1)}, {swarm, other}) {}
    std::vector<Peer> joinedPeers() {
        for (Peer* peer : {&a, &b, &c}) {
            peer->joinSwarm(swarm);
            peer->joinSwarm(other);
            // Fixture-only initialization on mutable Peers, before Network copies them.
            // No production piece-management API is needed for this feature.
            const_cast<PeerSwarmState&>(peer->swarmState(1)).localBitfield =
                {static_cast<std::uint8_t>(peer->id()), 0x80};
            const_cast<PeerSwarmState&>(peer->swarmState(2)).localBitfield =
                {static_cast<std::uint8_t>(0x80u >> peer->id())};
        }
        return {a, b, c};
    }
    void initiate(const Swarm& current, PeerId sender = 1, PeerId receiver = 2) {
        simulation.schedule(std::make_unique<SendMessageEvent>(
            simulation.currentTime(), network, current.id(), sender, receiver,
            handshake(current, network.peer(sender).protocolId())));
    }
    void checkExchange(SwarmId swarmId, PeerId first = 1, PeerId second = 2) const {
        const auto& firstState = network.peer(first).swarmState(swarmId);
        const auto& secondState = network.peer(second).swarmState(swarmId);
        const auto& forward = firstState.connections.at(second);
        const auto& reverse = secondState.connections.at(first);
        check(forward.handshakeComplete() && reverse.handshakeComplete());
        check(forward.bitfieldSent && reverse.bitfieldSent);
        check(forward.remoteBitfield == secondState.localBitfield);
        check(reverse.remoteBitfield == firstState.localBitfield);
    }
};

void automaticBitfieldExchange()
{
    AutoBitfieldFixture f;
    check(!PeerConnectionState{}.bitfieldSent);
    f.initiate(f.swarm);
    f.simulation.schedule(std::make_unique<CheckEvent>(0.001, [&] {
        check(!f.network.peer(1).swarmState(1).connections.at(2).bitfieldSent);
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(1.189, [&] {
        check(f.network.peer(2).swarmState(1).connections.at(1).bitfieldSent);
        check(!f.network.peer(1).swarmState(1).connections.at(2).bitfieldSent);
        check(f.network.transmissionState(2, 1).pendingCount == 1);
        // Duplicate handshakes while B's BITFIELD waits behind its response.
        f.network.deliver(1, 1, 2, handshake(f.swarm, f.a.protocolId()));
        f.network.deliver(1, 1, 2, handshake(f.swarm, f.a.protocolId()));
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(1.190, [&] {
        check(f.network.transmissionState(2, 1).pendingCount == 1);
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(2.377, [&] {
        const auto& a = f.network.peer(1).swarmState(1).connections.at(2);
        check(a.handshakeComplete() && a.bitfieldSent);
        check(a.remoteBitfield == std::vector<std::uint8_t>({0, 0}));
        f.network.deliver(1, 2, 1, handshake(f.swarm, f.b.protocolId()));
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(2.487, [&] {
        check(f.network.peer(1).swarmState(1).connections.at(2).remoteBitfield
            == std::vector<std::uint8_t>({0, 0}));
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(2.489, [&] {
        check(f.network.peer(1).swarmState(1).connections.at(2).remoteBitfield
            == f.b.swarmState(1).localBitfield);
        check(f.network.peer(2).swarmState(1).connections.at(1).remoteBitfield
            == std::vector<std::uint8_t>({0, 0}));
    }));
    f.simulation.run();
    f.checkExchange(1);
    // BITFIELDs arrive at 2.488 and 2.588; interest announcements finish at 2.768.
    // First periodic rechoke at t=10, then UNCHOKE transmission and propagation.
    check(std::abs(f.simulation.currentTime() - 10.18) < 1e-12);

    const auto aBefore = f.network.peer(1).swarmState(1);
    const auto bBefore = f.network.peer(2).swarmState(1);
    const double finished = f.simulation.currentTime();
    f.network.deliver(1, 1, 2, handshake(f.swarm, f.a.protocolId()));
    f.network.deliver(1, 2, 1, handshake(f.swarm, f.b.protocolId()));
    f.simulation.run();
    check(f.simulation.currentTime() == finished);
    checkUnchanged(f.network.peer(1).swarmState(1), aBefore);
    checkUnchanged(f.network.peer(2).swarmState(1), bBefore);
}

void automaticBitfieldScope()
{
    AutoBitfieldFixture f;
    // Default connections for the second swarm and another remote peer.
    f.network.deliver(2, 1, 2, Message(MessageType::Cancel, CancelPayload{}));
    f.network.deliver(1, 3, 2, Message(MessageType::Cancel, CancelPayload{}));
    f.initiate(f.swarm);
    f.simulation.run();
    f.checkExchange(1);
    check(!f.network.peer(2).swarmState(2).connections.at(1).bitfieldSent);
    check(!f.network.peer(2).swarmState(1).connections.at(3).bitfieldSent);
    const auto firstBefore = f.network.peer(2).swarmState(1);
    f.initiate(f.other);
    f.simulation.run();
    f.checkExchange(2);
    checkUnchanged(f.network.peer(2).swarmState(1), firstBefore);
    f.initiate(f.swarm, 3, 2);
    f.simulation.run();
    f.checkExchange(1, 3, 2);
    f.checkExchange(1);
    f.checkExchange(2);
}

void bitfieldDoesNotTriggerResponse()
{
    QueueFixture f; // Handshake flags set directly; no network completion event occurred.
    check(!f.remote().bitfieldSent);
    f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0x80}}));
    f.simulation.run();
    check(!f.remote().bitfieldSent);
    check(f.remote().remoteBitfield[0] == 0x80);
    check(f.simulation.currentTime() == 0);
    check(!f.network.transmissionState(2, 1).active);
    check(f.network.transmissionState(2, 1).pendingCount == 0);
}

void simultaneousHandshakeBitfields()
{
    AutoBitfieldFixture f;
    f.initiate(f.swarm);
    f.initiate(f.swarm, 2, 1);
    f.simulation.run();
    f.checkExchange(1);
    // First periodic rechoke at t=10, then UNCHOKE transmission and propagation.
    check(std::abs(f.simulation.currentTime() - 10.18) < 1e-12);
}
enum class ExecutedKind { Request, Start, Complete, Arrival };
struct ExecutedMessage {
    ExecutedKind kind;
    double time;
    MessageEventDetails details;
    unsigned bitfieldByte = 0;
};

void observeMessages(Simulation& simulation, std::vector<ExecutedMessage>& events)
{
    simulation.setEventObserver([&events](const Event& event) {
        if (const auto* request = dynamic_cast<const SendMessageEvent*>(&event)) {
            events.push_back({ExecutedKind::Request, event.time(), request->details()});
        } else if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event)) {
            unsigned byte = 0;
            if (start->message().type() == MessageType::Bitfield) {
                byte = std::get<BitfieldPayload>(start->message().payload()).bytes.at(0);
            }
            events.push_back({ExecutedKind::Start, event.time(), start->details(), byte});
        } else if (const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
            check(complete->details().has_value());
            events.push_back({ExecutedKind::Complete, event.time(), *complete->details()});
        } else if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event)) {
            events.push_back({ExecutedKind::Arrival, event.time(), arrival->details()});
        }
    });
}

void checkEventTimes(const std::vector<ExecutedMessage>& events, PeerId sender,
                     MessageType type, double request, double start, double complete, double arrival)
{
    const std::vector<ExecutedKind> kinds{
        ExecutedKind::Request, ExecutedKind::Start, ExecutedKind::Complete, ExecutedKind::Arrival};
    const std::vector<double> times{request, start, complete, arrival};
    std::size_t matched = 0;
    for (const auto& event : events) {
        if (event.details.sender != sender || event.details.messageType != type) continue;
        check(matched < kinds.size());
        check(event.kind == kinds[matched]);
        check(std::abs(event.time - times[matched]) < 1e-12);
        check(event.details.swarmId == 1);
        check(event.details.receiver == (sender == 1 ? 2 : 1));
        ++matched;
    }
    check(matched == kinds.size());
}

void exactTransmissionEventTimes()
{
    QueueFixture f(0.1);
    std::vector<ExecutedMessage> events;
    observeMessages(f.simulation, events);
    // 17 bytes at 800 bits/sec occupy A -> B from 5.0 through 5.17.
    f.send(5.0, 1, Message(MessageType::Cancel, CancelPayload{}));
    f.send(5.1, 1, Message(MessageType::Have, HavePayload{7}));
    f.simulation.run();
    checkEventTimes(events, 1, MessageType::Cancel, 5.0, 5.0, 5.17, 5.27);
    checkEventTimes(events, 1, MessageType::Have, 5.1, 5.17, 5.26, 5.36);
    check(f.remote().remoteBitfield[0] == 1);
    check(!f.network.transmissionState(1, 2).active);
}

void fifoTransmissionStartEvents()
{
    QueueFixture f;
    std::vector<ExecutedMessage> events;
    observeMessages(f.simulation, events);
    for (unsigned i = 0; i < 31; ++i) {
        f.send(i == 0 ? 5.0 : 5.01, 2, Message(MessageType::Bitfield,
            BitfieldPayload{{static_cast<std::uint8_t>(i + 1)}}));
    }
    f.simulation.run();
    unsigned requests = 0, starts = 0, completions = 0, arrivals = 0;
    for (const auto& event : events) {
        check(event.details.sender == 2 && event.details.receiver == 1);
        if (event.kind == ExecutedKind::Request) {
            check(std::abs(event.time - (requests == 0 ? 5.0 : 5.01)) < 1e-12);
            ++requests;
        } else if (event.kind == ExecutedKind::Start) {
            check(event.bitfieldByte == starts + 1);
            check(std::abs(event.time - (5.0 + starts * 0.06)) < 1e-12);
            ++starts;
        } else if (event.kind == ExecutedKind::Complete) {
            ++completions;
            check(std::abs(event.time - (5.0 + completions * 0.06)) < 1e-12);
        } else {
            ++arrivals;
            check(std::abs(event.time - (6.0 + arrivals * 0.06)) < 1e-12);
        }
    }
    check(requests == 31 && starts == 31 && completions == 31 && arrivals == 31);
    check(f.remote(1).remoteBitfield[0] == 31);
    check(!f.network.transmissionState(2, 1).active);
    check(f.network.transmissionState(2, 1).pendingCount == 0);
}

void simultaneousTransmissionStartEvents()
{
    QueueFixture f;
    std::vector<ExecutedMessage> events;
    observeMessages(f.simulation, events);
    f.send(5, 1, Message(MessageType::Have, HavePayload{0}));
    f.send(5, 2, Message(MessageType::Have, HavePayload{7}));
    f.simulation.run();
    checkEventTimes(events, 1, MessageType::Have, 5, 5, 5.09, 6.09);
    checkEventTimes(events, 2, MessageType::Have, 5, 5, 5.09, 6.09);
}

void automaticProtocolEventFlow()
{
    AutoBitfieldFixture f;
    std::vector<ExecutedMessage> events;
    observeMessages(f.simulation, events);
    f.simulation.schedule(std::make_unique<CheckEvent>(0, [&] { occupyDirection(f.network, 1, 2, 1); }));
    f.initiate(f.swarm);
    f.simulation.run();
    checkEventTimes(events, 1, MessageType::Handshake, 0, 0, 1.088, 1.188);
    checkEventTimes(events, 2, MessageType::Handshake, 1.188, 2.176, 3.264, 3.364);
    checkEventTimes(events, 2, MessageType::Bitfield, 2.176, 3.264, 3.376, 3.476);
    checkEventTimes(events, 1, MessageType::Bitfield, 3.364, 3.364, 3.476, 3.576);
    f.checkExchange(1);
}
struct InterestFixture {
    Swarm swarm{1, InfoHash{0x11}, 9};
    Swarm other{2, InfoHash{0x22}, 8};
    Peer a{1, 1000, 1000, PeerProtocolId{0xa1}};
    Peer b{2, 500, 500, PeerProtocolId{0xb2}};
    Peer c{3, 1000, 1000, PeerProtocolId{0xc3}};
    Simulation simulation;
    Network network;
    std::vector<ExecutedMessage> events;

    InterestFixture()
        : network(simulation, joinedPeers(), {Link(1, 2, 800, 0.1), Link(2, 3, 800, 0.1)},
                  {swarm, other}) { observeMessages(simulation, events); }
    std::vector<Peer> joinedPeers() {
        for (Peer* peer : {&a, &b, &c}) {
            peer->joinSwarm(swarm, peer == &b ? std::vector<std::uint8_t>{0x80, 0}
                                             : std::vector<std::uint8_t>{0xff, 0x80});
            peer->joinSwarm(other, peer == &b ? std::vector<std::uint8_t>{0x80}
                                             : std::vector<std::uint8_t>{0xff});
        }
        for (const Swarm* current : {&swarm, &other}) {
            for (Peer* remote : {&a, &c}) {
                b.markHandshakeSent(*current, remote->id());
                remote->markHandshakeSent(*current, b.id());
                b.receiveMessage(*current, remote->id(), handshake(*current, remote->protocolId()),
                                 &remote->protocolId());
                remote->receiveMessage(*current, b.id(), handshake(*current, b.protocolId()), &b.protocolId());
            }
        }
        return {a, b, c};
    }
    void receive(Message message, SwarmId swarmId = 1, PeerId sender = 1) {
        network.deliver(swarmId, sender, 2, message);
    }
    const PeerConnectionState& local(SwarmId swarmId = 1, PeerId remote = 1) const {
        return network.peer(2).swarmState(swarmId).connections.at(remote);
    }
    unsigned requests(MessageType type, SwarmId swarmId = 1, PeerId receiver = 1) const {
        return static_cast<unsigned>(std::count_if(events.begin(), events.end(), [&](const auto& event) {
            return event.kind == ExecutedKind::Request && event.details.messageType == type
                && event.details.swarmId == swarmId && event.details.sender == 2
                && event.details.receiver == receiver;
        }));
    }
};

void bitfieldInterestTransitions()
{
    InterestFixture f;
    check(!PeerConnectionState{}.weAreInterestedInRemote);
    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0x80, 0}})); // Already owned.
    check(!f.local().weAreInterestedInRemote);
    f.simulation.run();
    check(f.events.empty());

    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0x80, 0x80}})); // Missing piece 8.
    check(f.local().weAreInterestedInRemote && !f.local().remoteInterestedInUs);
    check(!f.network.peer(1).swarmState(1).connections.at(2).remoteInterestedInUs);
    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0x80, 0x80}}));
    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0x01, 0}}));
    f.simulation.run();
    check(f.requests(MessageType::Interested) == 1);
    check(f.network.peer(1).swarmState(1).connections.at(2).remoteInterestedInUs);

    f.receive(Message(MessageType::Interested));
    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0x80, 0}}));
    check(!f.local().weAreInterestedInRemote && f.local().remoteInterestedInUs);
    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0, 0}}));
    f.simulation.run();
    check(f.requests(MessageType::NotInterested) == 1);
    check(f.requests(MessageType::Interested) == 1);
    check(!f.network.peer(1).swarmState(1).connections.at(2).remoteInterestedInUs);
    check(f.local().remoteInterestedInUs); // The two interest directions are independent.
}

void haveInterestTransitions()
{
    InterestFixture f;
    f.receive(Message(MessageType::Have, HavePayload{0})); // Owned locally.
    check(!f.local().weAreInterestedInRemote);
    f.receive(Message(MessageType::Have, HavePayload{8}));
    check(f.local().weAreInterestedInRemote);
    check(f.local().remoteBitfield == std::vector<std::uint8_t>({0x80, 0x80}));
    f.receive(Message(MessageType::Have, HavePayload{8}));
    f.receive(Message(MessageType::Have, HavePayload{7}));
    f.simulation.run();
    check(f.requests(MessageType::Interested) == 1);
    check(f.requests(MessageType::NotInterested) == 0);
    check(f.network.peer(1).swarmState(1).connections.at(2).remoteInterestedInUs);
}

void interestScope()
{
    InterestFixture f;
    f.receive(Message(MessageType::Have, HavePayload{8}));
    check(f.local().weAreInterestedInRemote);
    check(!f.local(2).weAreInterestedInRemote);
    check(!f.local(1, 3).weAreInterestedInRemote);
    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0x40}}), 2);
    f.receive(Message(MessageType::Have, HavePayload{7}), 1, 3);
    f.simulation.run();
    check(f.local(2).weAreInterestedInRemote && f.local(1, 3).weAreInterestedInRemote);
    check(f.requests(MessageType::Interested) == 1);
    check(f.requests(MessageType::Interested, 2) == 1);
    check(f.requests(MessageType::Interested, 1, 3) == 1);
    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0, 0}}));
    f.simulation.run();
    check(!f.local().weAreInterestedInRemote);
    check(f.local(2).weAreInterestedInRemote && f.local(1, 3).weAreInterestedInRemote);
    check(f.requests(MessageType::NotInterested) == 1);
    check(f.requests(MessageType::NotInterested, 2) == 0);
    check(f.requests(MessageType::NotInterested, 1, 3) == 0);
}

void interestUsesDirectionalFifo()
{
    InterestFixture f;
    occupyDirection(f.network, 1, 2, 1);
    f.receive(Message(MessageType::Have, HavePayload{8}));
    f.receive(Message(MessageType::Bitfield, BitfieldPayload{{0, 0}}));
    f.simulation.schedule(std::make_unique<CheckEvent>(0.001, [&] {
        check(f.network.transmissionState(2, 1).pendingCount == 9);
        check(!f.local().weAreInterestedInRemote);
        check(!f.network.peer(1).swarmState(1).connections.at(2).remoteInterestedInUs);
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(2.357, [&] {
        check(f.network.peer(1).swarmState(1).connections.at(2).remoteInterestedInUs);
    }));
    f.simulation.run();
    checkEventTimes(f.events, 2, MessageType::Interested, 0, 2.176, 2.256, 2.356);
    checkEventTimes(f.events, 2, MessageType::NotInterested, 0, 2.256, 2.336, 2.436);
    check(!f.network.peer(1).swarmState(1).connections.at(2).remoteInterestedInUs);
    check(!f.network.transmissionState(2, 1).active);
}

void invalidAvailabilityPreservesInterest()
{
    InterestFixture f;
    for (bool interested : {false, true}) {
        if (interested) {
            f.receive(Message(MessageType::Have, HavePayload{8}));
            f.simulation.run();
        }
        const auto before = f.network.peer(2).swarmState(1);
        const auto count = f.events.size();
        for (const auto& message : {
                Message(MessageType::Have, HavePayload{9}),
                Message(MessageType::Bitfield, BitfieldPayload{{0}}),
                Message(MessageType::Bitfield, BitfieldPayload{{0, 1}})}) {
            rejects([&] { f.receive(message); });
            checkUnchanged(f.network.peer(2).swarmState(1), before);
        }
        f.simulation.run();
        check(f.events.size() == count);
    }
}

void initialLocalBitfield()
{
    const Swarm swarm(1, {}, 9);
    Peer peer(1);
    for (const auto& bytes : {std::vector<std::uint8_t>{}, std::vector<std::uint8_t>{0},
                             std::vector<std::uint8_t>{0, 1}}) {
        rejects([&] { peer.joinSwarm(swarm, bytes); });
        check(!peer.hasSwarm(1));
    }
    peer.joinSwarm(swarm, {0x80, 0x80});
    check(peer.swarmState(1).localBitfield == std::vector<std::uint8_t>({0x80, 0x80}));
    rejects([&] { peer.joinSwarm(swarm, {0, 0}); });
    check(peer.swarmState(1).localBitfield == std::vector<std::uint8_t>({0x80, 0x80}));
}
void chokePolicyAndArrival()
{
    InterestFixture f;
    check(PeerConnectionState{}.weAreChokingRemote && PeerConnectionState{}.remoteIsChokingUs);
    f.receive(Message(MessageType::NotInterested));
    f.simulation.run(); check(f.events.empty());
    f.receive(Message(MessageType::Interested));
    f.receive(Message(MessageType::Interested));
    check(f.local().weAreChokingRemote);
    f.simulation.schedule(std::make_unique<CheckEvent>(10.081, [&] {
        check(!f.local().weAreChokingRemote);
        check(f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(11, [&] {
        f.receive(Message(MessageType::Interested)); // Duplicate decision stays suppressed.
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(12, [&] {
        f.receive(Message(MessageType::NotInterested));
        f.receive(Message(MessageType::NotInterested));
        check(!f.local().weAreChokingRemote); // Choke waits for t=20.
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(20.081, [&] {
        check(f.local().weAreChokingRemote);
        check(!f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    }));
    f.simulation.run();
    checkEventTimes(f.events, 2, MessageType::Unchoke, 10, 10, 10.08, 10.18);
    checkEventTimes(f.events, 2, MessageType::Choke, 20, 20, 20.08, 20.18);
    check(f.requests(MessageType::Choke) == 1 && f.requests(MessageType::Unchoke) == 1);
    check(f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
}
void chokePolicyScope()
{
    InterestFixture f;
    f.receive(Message(MessageType::Interested));
    f.simulation.run();
    check(!f.local().weAreChokingRemote && f.local().remoteIsChokingUs);
    check(f.local(2).weAreChokingRemote && f.local(1, 3).weAreChokingRemote);
    check(f.network.peer(1).swarmState(1).connections.at(2).weAreChokingRemote);
    f.receive(Message(MessageType::Interested), 2);
    f.receive(Message(MessageType::Interested), 1, 3);
    f.network.deliver(1, 2, 1, Message(MessageType::Interested)); // Reverse policy direction.
    f.simulation.run();
    check(!f.local().remoteIsChokingUs);
    check(!f.local(2).weAreChokingRemote && !f.local(1, 3).weAreChokingRemote);
    f.receive(Message(MessageType::NotInterested));
    f.simulation.run();
    check(f.local().weAreChokingRemote && !f.local().remoteIsChokingUs);
    check(!f.local(2).weAreChokingRemote && !f.local(1, 3).weAreChokingRemote);
    check(f.requests(MessageType::Unchoke) == 1);
    check(f.requests(MessageType::Unchoke, 2) == 1);
    check(f.requests(MessageType::Unchoke, 1, 3) == 1);
    check(f.requests(MessageType::Choke) == 1);
}

void chokePolicyUsesFifo()
{
    InterestFixture f;
    f.receive(Message(MessageType::Interested));
    f.simulation.schedule(std::make_unique<CheckEvent>(9, [&] { occupyDirection(f.network, 1, 2, 1); }));
    f.simulation.schedule(std::make_unique<CheckEvent>(10.001, [&] {
        check(f.network.transmissionState(2, 1).pendingCount > 0);
        check(f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(12, [&] { f.receive(Message(MessageType::NotInterested)); }));
    f.simulation.schedule(std::make_unique<CheckEvent>(19, [&] { occupyDirection(f.network, 1, 2, 1); }));
    f.simulation.run();
    checkEventTimes(f.events, 2, MessageType::Unchoke, 10, 11.176, 11.256, 11.356);
    checkEventTimes(f.events, 2, MessageType::Choke, 20, 21.176, 21.256, 21.356);
    check(f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
}struct RequestFixture {
    Swarm swarm{1, InfoHash{0x51}, 2500, 1024};
    Swarm other{2, InfoHash{0x52}, 1500, 1024};
    Peer a{1, 1000, 1000, PeerProtocolId{0xa1}};
    Peer b{2, 500, 500, PeerProtocolId{0xb2}};
    Peer c{3, 1000, 1000, PeerProtocolId{0xc3}};
    Simulation simulation;
    Network network;
    std::vector<ExecutedMessage> events;

    RequestFixture(bool ownsFirst = true, bool ownsSecond = false)
        : network(simulation, joinedPeers(ownsFirst, ownsSecond), {Link(1, 2, 800, .1), Link(2, 3, 800, .1)},
                  {swarm, other}) {
        observeMessages(simulation, events);
    }
    std::vector<Peer> joinedPeers(bool ownsFirst, bool ownsSecond) {
        for (Peer* peer : {&a, &b, &c}) {
            peer->joinSwarm(swarm, {static_cast<std::uint8_t>(peer == &b ? 0x40 : (0x20 | (ownsFirst ? 0x80 : 0) | (ownsSecond ? 0x40 : 0)))});
            peer->joinSwarm(other, {static_cast<std::uint8_t>(peer == &b ? 0x40 : 0x80)});
        }
        // These tests explicitly control block traffic. Establish protocol state directly
        // and leave availability unadvertised, so the automatic selector has no candidates.
        for (const auto* current : {&swarm, &other}) {
            for (Peer* remote : {&a, &c}) {
                b.markHandshakeSent(*current, remote->id());
                remote->markHandshakeSent(*current, b.id());
                b.receiveMessage(*current, remote->id(), handshake(*current, remote->protocolId()), &remote->protocolId());
                remote->receiveMessage(*current, b.id(), handshake(*current, b.protocolId()), &b.protocolId());
                for (const auto pair : {std::pair{&b, remote}, std::pair{remote, &b}}) {
                    auto& connection = const_cast<PeerSwarmState&>(pair.first->swarmState(current->id())).connections.at(pair.second->id());
                    connection.bitfieldSent = true;
                    connection.weAreInterestedInRemote = true;
                    connection.remoteInterestedInUs = true;
                    connection.remoteIsChokingUs = false;
                    connection.weAreChokingRemote = false;
                }
            }
        }
        return {a, b, c};
    }
    const PeerConnectionState& outgoing(SwarmId id = 1, PeerId remote = 1) const {
        return network.peer(2).swarmState(id).connections.at(remote);
    }
    const PeerConnectionState& incoming(SwarmId id = 1, PeerId remote = 1) const {
        return network.peer(remote).swarmState(id).connections.at(2);
    }
    void rejectRequest(RequestPayload request) {
        const auto beforeA = network.peer(1).swarmState(1);
        const auto beforeB = network.peer(2).swarmState(1);
        const auto queue = network.transmissionState(2, 1);
        rejects([&] { network.send(1, 2, 1, Message(MessageType::Request, request)); });
        checkUnchanged(network.peer(1).swarmState(1), beforeA);
        checkUnchanged(network.peer(2).swarmState(1), beforeB);
        check(network.transmissionState(2, 1).active == queue.active);
        check(network.transmissionState(2, 1).pendingCount == queue.pendingCount);
    }
};

void swarmPieceSizes()
{
    const Swarm shorter(1, {}, 2500, 1024);
    check(shorter.totalSize() == 2500 && shorter.pieceLength() == 1024);
    check(shorter.pieceCount() == 3);
    check(shorter.pieceSize(0) == 1024 && shorter.pieceSize(1) == 1024);
    check(shorter.pieceSize(2) == 452);
    check(Swarm(1, {}, 2048, 1024).pieceSize(1) == 1024);
    check(Swarm(1, {}, 7, 1024).pieceSize(0) == 7);
    rejects([&] { shorter.pieceSize(3); });
    rejects([] { Swarm(1, {}, 0, 1024); });
    rejects([] { Swarm(1, {}, 100, 0); });
    rejects([] { Swarm(1, {}, std::numeric_limits<std::uint64_t>::max(), 1); });
    rejects([] { Swarm(1, {}, 3).pieceSize(0); });
}

void validRequestAndDuplicates()
{
    RequestFixture f;
    const RequestPayload block{2, 400, 52};
    const Message request(MessageType::Request, block);
    check(request.wireSize() == 17);
    f.network.send(1, 2, 1, request);
    f.network.send(1, 2, 1, request);
    check(f.outgoing().outgoingRequests == std::vector<RequestPayload>{block});
    check(f.incoming().acceptedRequests.empty());
    const double start = f.simulation.currentTime();
    f.simulation.schedule(std::make_unique<CheckEvent>(start + .373, [&] {
        check(f.incoming().acceptedRequests == std::vector<RequestPayload>{block});
        f.network.deliver(1, 2, 1, request); // Duplicate while response is in flight.
    }));
    f.simulation.run();
    check(f.incoming().acceptedRequests.empty() && f.outgoing().outgoingRequests.empty());
    check(std::count_if(f.events.begin(), f.events.end(), [](const auto& event) {
        return event.kind == ExecutedKind::Start && event.details.messageType == MessageType::Request;
    }) == 1);
    check(std::count_if(f.events.begin(), f.events.end(), [](const auto& event) {
        return event.kind == ExecutedKind::Start && event.details.messageType == MessageType::Piece;
    }) == 1);
}
void invalidRequestRanges()
{
    RequestFixture f;
    for (const auto request : {RequestPayload{3, 0, 1}, RequestPayload{0, 0, 0},
            RequestPayload{0, 1024, 1}, RequestPayload{0, 1023, 2},
            RequestPayload{2, 451, 2}, RequestPayload{2, 452, 1},
            RequestPayload{2, 400, 53}, RequestPayload{0, 1, 0xffffffffu},
            RequestPayload{0, 0xffffffffu, 2}}) {
        f.rejectRequest(request);
        const auto before = f.network.peer(1).swarmState(1);
        rejects([&] { f.network.deliver(1, 2, 1, Message(MessageType::Request, request)); });
        checkUnchanged(f.network.peer(1).swarmState(1), before);
    }
}

void requestConnectionAndOwnershipValidation()
{
    {
        RequestFixture f;
        f.network.deliver(1, 1, 2, Message(MessageType::Choke));
        f.rejectRequest({0, 0, 1});
    }
    {
        RequestFixture f;
        f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0}}));
        f.rejectRequest({0, 0, 1}); // Not interested.
    }
    {
        RequestFixture f(true, true);
        f.rejectRequest({1, 0, 1}); // Both own it: local ownership alone rejects.
    }
    {
        RequestFixture f(false);
        // Advertised availability cannot substitute for actual remote ownership.
        f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0xa0}}));
        f.rejectRequest({0, 0, 1}); // Neither owns piece 0; piece 2 keeps us interested.
    }
    {
        const Swarm swarm(1, {}, 2048, 1024);
        Peer a(1, 1000, 1000), b(2, 1000, 1000);
        a.joinSwarm(swarm, {0x80});
        Simulation simulation;
        Network network(simulation, {a, b}, {Link(1, 2, 1000, .1)}, {swarm});
        rejects([&] { network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 1})); });
        check(!network.peer(2).hasSwarm(1));
        b.joinSwarm(swarm);
        Network joined(simulation, {a, b}, {Link(1, 2, 1000, .1)}, {swarm});
        rejects([&] { joined.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 1})); });
        check(joined.peer(2).swarmState(1).connections.empty()); // No handshake.
    }
}

void requestScopeAndDistinctBlocks()
{
    RequestFixture f;
    for (SwarmId id : {1u, 2u}) {
        for (PeerId remote : {1u, 3u}) {
            f.network.send(id, 2, remote, Message(MessageType::Request, RequestPayload{0, 0, 16}));
        }
    }
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 16, 16}));
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 8}));
    f.simulation.run();
    check(f.incoming().acceptedRequests.empty());
    check(f.outgoing().outgoingRequests.empty());
    check(f.incoming(2).acceptedRequests.empty());
    check(f.incoming(1, 3).acceptedRequests.empty());
    check(f.incoming(2, 3).acceptedRequests.empty());
    check(f.network.peer(2).swarmState(1).connections.at(1).acceptedRequests.empty());
}

void requestFifoArrival()
{
    RequestFixture f;
    const double start = f.simulation.currentTime();
    occupyDirection(f.network, 1, 2, 1);
    f.simulation.schedule(std::make_unique<SendMessageEvent>(start, f.network, 1, 2, 1,
        Message(MessageType::Request, RequestPayload{0, 0, 16})));
    f.simulation.schedule(std::make_unique<CheckEvent>(start + 2.449, [&] {
        check(f.incoming().acceptedRequests.empty()); // TX done; still propagating.
    }));
    f.simulation.run();
    checkEventTimes(f.events, 2, MessageType::Request, start, start + 2.176, start + 2.448, start + 2.548);
    check(f.incoming().acceptedRequests.empty());
}

void requestArrivalRevalidation()
{
    RequestFixture f;
    const double start = f.simulation.currentTime();
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 16}));
    // The receiver chokes the requester after enqueue but before arrival.
    f.network.deliver(1, 2, 1, Message(MessageType::NotInterested));
    PeerSwarmState beforeA, beforeB;
    f.simulation.schedule(std::make_unique<CheckEvent>(start + .371, [&] {
        beforeA = f.network.peer(1).swarmState(1);
        beforeB = f.network.peer(2).swarmState(1);
    }));
    rejects([&] { f.simulation.run(); });
    check(f.incoming().acceptedRequests.empty());
    checkUnchanged(f.network.peer(1).swarmState(1), beforeA);
    checkUnchanged(f.network.peer(2).swarmState(1), beforeB);
}
void piecePayloadTimingAndFifo()
{
    RequestFixture f;
    const double start = f.simulation.currentTime();
    check(Message(MessageType::Piece, PiecePayload{0, 0, 128}).wireSize() == 141);
    check(Message(MessageType::Piece, PiecePayload{0, 0, 0xffffffffu}).wireSize()
        == std::uint64_t(13) + 0xffffffffu);
    occupyDirection(f.network, 1, 1, 2);
    f.simulation.schedule(std::make_unique<SendMessageEvent>(start, f.network, 1, 2, 1,
        Message(MessageType::Request, RequestPayload{0, 0, 128})));
    f.simulation.schedule(std::make_unique<CheckEvent>(start + .373, [&] {
        check(f.outgoing().outgoingRequests.size() == 1);
        check(f.incoming().acceptedRequests.size() == 1);
        check(f.network.peer(2).swarmState(1).receivedBlocks.empty());
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(start + 4.433, [&] {
        check(f.outgoing().outgoingRequests.size() == 1);
        check(f.network.peer(2).swarmState(1).receivedBlocks.empty());
    }));
    f.simulation.run();
    checkEventTimes(f.events, 1, MessageType::Piece,
        start + .372, start + 2.176, start + 4.432, start + 4.532);
    check(f.outgoing().outgoingRequests.empty() && f.incoming().acceptedRequests.empty());
    const auto& state = f.network.peer(2).swarmState(1);
    check(state.receivedBlocks.at(0) == std::vector<BlockRange>{{0, 128}});
    check(state.localBitfield[0] == 0x40);
}

void overlappingBlocksAndCompletion()
{
    RequestFixture f;
    for (const auto block : {RequestPayload{0, 0, 600}, RequestPayload{0, 400, 400}}) {
        f.network.send(1, 2, 1, Message(MessageType::Request, block));
    }
    f.simulation.run();
    const auto& state = f.network.peer(2).swarmState(1);
    check(state.receivedBlocks.at(0) == std::vector<BlockRange>{{0, 800}});
    check(state.localBitfield[0] == 0x40); // 1000 transferred bytes cover only 800 unique bytes.
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 900, 124}));
    f.simulation.run();
    check(state.receivedBlocks.at(0) == std::vector<BlockRange>({{0, 800}, {900, 1024}}));
    check(state.localBitfield[0] == 0x40); // End reached but a gap remains.
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 800, 100}));
    f.simulation.run();
    check(state.receivedBlocks.at(0) == std::vector<BlockRange>{{0, 1024}});
    check(state.localBitfield[0] == 0xc0);
    check(f.outgoing().outgoingRequests.empty());
}

void sixteenBlocksAndShortFinalPiece()
{
    RequestFixture f;
    for (unsigned i = 0; i < 16; ++i) {
        f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, i * 64, 64}));
    }
    for (unsigned i = 0; i < 2; ++i) {
        f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{2, i * 226, 226}));
    }
    // Keep interest accurate while the manually requested final piece is still pending.
    const_cast<PeerSwarmState&>(f.network.peer(2).swarmState(1)).connections.at(1).remoteBitfield[0] = 0x20;
    f.simulation.run();
    const auto& state = f.network.peer(2).swarmState(1);
    check(state.receivedBlocks.at(0) == std::vector<BlockRange>{{0, 1024}});
    check(state.receivedBlocks.at(2) == std::vector<BlockRange>{{0, 452}});
    check(state.localBitfield[0] == 0xe0);
    check(f.outgoing().outgoingRequests.empty());
}

void rejectedPiecePreservesState()
{
    RequestFixture f;
    auto reject = [&](PiecePayload piece, PeerId sender = 1) {
        const auto before = f.network.peer(2).swarmState(1);
        const auto remoteBefore = f.network.peer(sender).swarmState(1);
        rejects([&] { f.network.deliver(1, sender, 2, Message(MessageType::Piece, piece)); });
        checkUnchanged(f.network.peer(2).swarmState(1), before);
        checkUnchanged(f.network.peer(sender).swarmState(1), remoteBefore);
    };
    reject({0, 0, 128}); // Unsolicited.
    rejects([&] { f.network.send(1, 1, 2, Message(MessageType::Piece, PiecePayload{0, 0, 128})); });
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 128}));
    reject({0, 0, 128}, 3); // Correct tuple, wrong remote peer.
    for (const auto piece : {PiecePayload{3, 0, 1}, PiecePayload{0, 0, 0},
            PiecePayload{0, 1024, 1}, PiecePayload{2, 400, 53},
            PiecePayload{0, 1, 0xffffffffu}, PiecePayload{0, 1, 127},
            PiecePayload{0, 0, 129}}) reject(piece);
    f.simulation.run();
    reject({0, 0, 128}); // Duplicate after the matching request was removed.
    check(f.network.peer(2).swarmState(1).receivedBlocks.at(0) == std::vector<BlockRange>{{0, 128}});
}

void pieceSwarmAndRemoteScope()
{
    RequestFixture f;
    // The same piece may legitimately combine nonoverlapping blocks from two peers.
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 512}));
    f.network.send(1, 2, 3, Message(MessageType::Request, RequestPayload{0, 512, 512}));
    f.network.send(2, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 16}));
    f.simulation.run();
    check(f.network.peer(2).swarmState(1).receivedBlocks.at(0) == std::vector<BlockRange>{{0, 1024}});
    check(f.network.peer(2).swarmState(1).localBitfield[0] == 0xc0);
    check(f.network.peer(2).swarmState(2).receivedBlocks.at(0) == std::vector<BlockRange>{{0, 16}});
    check(f.network.peer(2).swarmState(2).localBitfield[0] == 0x40);
    check(f.outgoing().outgoingRequests.empty() && f.outgoing(1, 3).outgoingRequests.empty());
    check(f.network.peer(1).swarmState(1).receivedBlocks.empty());
    check(f.network.peer(3).swarmState(1).receivedBlocks.empty());
}

void pieceRevalidatesAtTransmissionStart()
{
    RequestFixture f;
    const double start = f.simulation.currentTime();
    occupyDirection(f.network, 1, 1, 2);
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 128}));
    f.simulation.schedule(std::make_unique<CheckEvent>(start + .5, [&] {
        check(f.incoming().acceptedRequests.size() == 1); // PIECE already queued.
        // Bypass policy scheduling to exercise defensive transmission-start validation.
        const_cast<PeerConnectionState&>(f.incoming()).weAreChokingRemote = true;
    }));
    rejects([&] { f.simulation.run(); });
    check(f.network.peer(2).swarmState(1).receivedBlocks.empty());
    check(f.outgoing().outgoingRequests.size() == 1);
}
void pieceRequiresEstablishedSenderContext()
{
    RequestFixture f;
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 128}));
    Peer receiver = f.network.peer(2);
    Peer sender(1, 1000, 1000);
    sender.joinSwarm(f.swarm, {0xa0});
    const auto before = receiver.swarmState(1);
    const Message piece(MessageType::Piece, PiecePayload{0, 0, 128});
    rejects([&] { receiver.receiveMessage(f.swarm, 1, piece); });
    rejects([&] { receiver.receiveMessage(f.swarm, 1, piece, nullptr, &sender); });
    checkUnchanged(receiver.swarmState(1), before);
    f.simulation.run();
    check(f.outgoing().outgoingRequests.empty() && f.incoming().acceptedRequests.empty());
}

void pieceInFlightSurvivesChoking()
{
    RequestFixture f;
    const double start = f.simulation.currentTime();
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 128}));
    f.simulation.schedule(std::make_unique<CheckEvent>(start + .5, [&] {
        check(f.network.transmissionState(1, 2).active);
        check(f.incoming().acceptedRequests.size() == 1);
        f.network.deliver(1, 2, 1, Message(MessageType::NotInterested));
        f.network.deliver(1, 1, 2, Message(MessageType::Choke));
    }));
    f.simulation.run();
    check(f.outgoing().outgoingRequests.empty() && f.incoming().acceptedRequests.empty());
    check(f.network.peer(2).swarmState(1).receivedBlocks.at(0) == std::vector<BlockRange>{{0, 128}});
    check(f.network.peer(2).swarmState(1).localBitfield[0] == 0x40);
}

void queuedPieceRevalidatesOutstandingRequest()
{
    RequestFixture f;
    const double start = f.simulation.currentTime();
    occupyDirection(f.network, 1, 1, 2);
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 128}));
    f.simulation.schedule(std::make_unique<CheckEvent>(start + .5, [&] {
        check(f.incoming().acceptedRequests.size() == 1);
        // Deliver the matching response while its queued copy still waits for bandwidth.
        f.network.deliver(1, 1, 2, Message(MessageType::Piece, PiecePayload{0, 0, 128}));
        check(f.outgoing().outgoingRequests.empty() && f.incoming().acceptedRequests.empty());
    }));
    rejects([&] { f.simulation.run(); });
    check(!f.network.transmissionState(1, 2).active);
    check(f.network.peer(2).swarmState(1).receivedBlocks.at(0) == std::vector<BlockRange>{{0, 128}});
    check(std::none_of(f.events.begin(), f.events.end(), [](const auto& event) {
        return event.details.messageType == MessageType::Piece
            && (event.kind == ExecutedKind::Complete || event.kind == ExecutedKind::Arrival);
    }));
}

void completionHaveScopeFifoAndInterest()
{
    const Swarm swarm(1, InfoHash{1}, 20, 16); // Four-byte final piece.
    const Swarm other(2, InfoHash{2}, 20, 16);
    // Keep this protocol/FIFO regression link-limited with noncontended peer budgets.
    std::vector<Peer> peers;
    for (PeerId id = 1; id <= 6; ++id) {
        peers.emplace_back(id, 1500, 1500, PeerProtocolId{static_cast<std::uint8_t>(id)});
        peers.back().joinSwarm(swarm);
        peers.back().joinSwarm(other);
    }
    peers[1].markHandshakeSent(swarm, 5); // Incomplete connection must not receive HAVE.
    Simulation simulation;
    Network network(simulation, peers,
        {Link(2, 1, 500, .1), Link(2, 3, 500, .1), Link(2, 4, 500, .1),
         Link(2, 5, 500, .1), Link(2, 6, 500, .1)}, {swarm, other});
    for (PeerId remote : {1u, 3u}) network.send(1, 2, remote, handshake(swarm, peers[1].protocolId()));
    network.send(2, 2, 4, handshake(other, peers[1].protocolId()));
    simulation.run();
    std::vector<ExecutedMessage> events;
    observeMessages(simulation, events);
    const double start = simulation.currentTime();
    auto& seed = const_cast<PeerSwarmState&>(network.peer(1).swarmState(1));
    auto& downloader = const_cast<PeerSwarmState&>(network.peer(2).swarmState(1));
    seed.localBitfield[0] = 0x40;
    seed.connections.at(2).remoteInterestedInUs = true;
    seed.connections.at(2).weAreChokingRemote = false;
    downloader.connections.at(1).remoteBitfield[0] = 0x40;
    downloader.connections.at(1).weAreInterestedInRemote = true;
    network.deliver(1, 1, 2, Message(MessageType::Unchoke)); // Automatically requests the final piece.
    simulation.schedule(std::make_unique<CheckEvent>(start + .4, [&] {
        occupyDirection(network, 1, 2, 1); // HAVE must wait behind existing traffic.
    }));
    simulation.schedule(std::make_unique<CheckEvent>(start + .745, [&] {
        check(network.peer(2).swarmState(1).localBitfield[0] == 0x40);
        check(network.peer(1).swarmState(1).connections.at(2).remoteBitfield[0] == 0);
        check(network.peer(3).swarmState(1).connections.at(2).remoteBitfield[0] == 0);
    }));
    simulation.schedule(std::make_unique<CheckEvent>(start + .989, [&] {
        const auto& observer = network.peer(3).swarmState(1).connections.at(2);
        check(observer.remoteBitfield[0] == 0x40 && observer.weAreInterestedInRemote);
        check(network.peer(1).swarmState(1).connections.at(2).remoteBitfield[0] == 0);
    }));
    simulation.run();
    std::vector<ExecutedMessage> toSeed;
    std::vector<PeerId> recipients;
    for (const auto& event : events) {
        if (event.details.messageType == MessageType::Have && event.details.sender == 2) {
            check(event.details.sender == 2 && event.details.swarmId == 1);
            check(event.details.receiver == 1 || event.details.receiver == 3);
            if (event.kind == ExecutedKind::Request) recipients.push_back(event.details.receiver);
            if (event.details.receiver == 1) toSeed.push_back(event);
        }
        if (event.details.messageType == MessageType::Request) check(event.details.sender == 2 || event.details.sender == 3);
    }
    check(recipients == std::vector<PeerId>({1, 3}));
    checkEventTimes(toSeed, 2, MessageType::Have,
        start + .744, start + 2.576, start + 2.720, start + 2.820);
    check(network.peer(1).swarmState(1).connections.at(2).remoteBitfield[0] == 0x40);
    check(!network.peer(2).swarmState(1).connections.at(3).remoteInterestedInUs);
    check(network.peer(3).swarmState(1).connections.at(2).remoteIsChokingUs);
    check(network.peer(3).swarmState(1).connections.at(2).outgoingRequests.empty());
    check(network.peer(4).swarmState(2).connections.at(2).remoteBitfield[0] == 0);
    check(network.peer(5).swarmState(1).connections.empty());
    check(network.peer(6).swarmState(1).connections.empty());
}

void completionHaveOnlyOnFirstCompletion()
{
    RequestFixture f;
    f.network.send(1, 2, 1, Message(MessageType::Request, RequestPayload{0, 0, 512}));
    f.simulation.run();
    check(std::none_of(f.events.begin(), f.events.end(), [](const auto& event) {
        return event.details.messageType == MessageType::Have;
    }));
    // Both responses are outstanding before completion. The second is valid redundant data.
    for (PeerId remote : {1u, 3u}) {
        f.network.send(1, 2, remote, Message(MessageType::Request, RequestPayload{0, 512, 512}));
    }
    f.simulation.run();
    check(f.network.peer(2).swarmState(1).localBitfield[0] == 0xc0);
    check(f.outgoing().outgoingRequests.empty() && f.outgoing(1, 3).outgoingRequests.empty());
    check(f.incoming().acceptedRequests.empty() && f.incoming(1, 3).acceptedRequests.empty());
    for (PeerId remote : {1u, 3u}) {
        check(std::count_if(f.events.begin(), f.events.end(), [remote](const auto& event) {
            return event.kind == ExecutedKind::Request && event.details.messageType == MessageType::Have
                && event.details.sender == 2 && event.details.receiver == remote && event.details.swarmId == 1;
        }) == 1);
        check(f.incoming(1, remote).remoteBitfield[0] == 0x80);
        check(f.incoming(2, remote).remoteBitfield[0] == 0);
    }
    const auto eventCount = f.events.size();
    rejects([&] { f.network.deliver(1, 1, 2, Message(MessageType::Piece, PiecePayload{0, 512, 512})); });
    f.simulation.run();
    check(f.events.size() == eventCount);
}

void autonomousSequentialPipeline()
{
    constexpr std::uint32_t pieceLength = 4 * 16384 + 13;
    const Swarm swarm(1, InfoHash{1}, 2 * pieceLength + 101, pieceLength);
    Peer seed(1, 1000000, 1000000, PeerProtocolId{1}), leecher(2, 1000000, 1000000, PeerProtocolId{2});
    seed.joinSwarm(swarm, {0xe0});
    leecher.joinSwarm(swarm);
    Simulation simulation;
    Network network(simulation, {seed, leecher}, {Link(1, 2, 1000000, .01)}, {swarm});
    std::vector<RequestPayload> started;
    std::size_t maxPending = 0;
    unsigned sends = 0, completes = 0, arrivals = 0, pieceArrivals = 0;
    bool refilledBeforeSecondPieceArrival = false;
    simulation.setEventObserver([&](const Event& event) {
        const auto& connections = network.peer(2).swarmState(1).connections;
        if (connections.contains(1)) {
            const auto& connection = connections.at(1);
            const auto pending = connection.outgoingRequests.size() + connection.scheduledRequests.size();
            check(pending <= 5);
            maxPending = std::max(maxPending, pending);
        }
        if (const auto* send = dynamic_cast<const SendMessageEvent*>(&event);
            send && send->details().messageType == MessageType::Request) {
            ++sends;
            const auto& connection = connections.at(1);
            check(connection.handshakeComplete() && connection.weAreInterestedInRemote && !connection.remoteIsChokingUs);
            if (pieceArrivals == 1) refilledBeforeSecondPieceArrival = true;
        }
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
            start && start->details().messageType == MessageType::Request) {
            started.push_back(std::get<RequestPayload>(start->message().payload()));
        }
        if (const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event);
            complete && complete->details()->messageType == MessageType::Request) ++completes;
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event)) {
            if (arrival->details().messageType == MessageType::Request) ++arrivals;
            if (arrival->details().messageType == MessageType::Piece) ++pieceArrivals;
        }
    });
    network.send(1, 2, 1, handshake(swarm, leecher.protocolId()));
    simulation.run();
    std::vector<RequestPayload> expected;
    for (std::uint32_t piece = 0; piece < 2; ++piece) {
        for (std::uint32_t block = 0; block < 4; ++block) expected.push_back({piece, block * 16384, 16384});
        expected.push_back({piece, 65536, 13});
    }
    expected.push_back({2, 0, 101});
    check(started == expected);
    check(maxPending == 5 && refilledBeforeSecondPieceArrival);
    check(sends == 11 && completes == 11 && arrivals == 11 && pieceArrivals == 11);
    const auto& state = network.peer(2).swarmState(1);
    check(state.localBitfield == std::vector<std::uint8_t>{0xe0});
    check(state.connections.at(1).scheduledRequests.empty() && state.connections.at(1).outgoingRequests.empty());
    check(network.peer(1).swarmState(1).connections.at(2).acceptedRequests.empty());
    check(network.peer(1).swarmState(1).connections.at(2).remoteBitfield == state.localBitfield);
    check(!state.connections.at(1).weAreInterestedInRemote);
    check(!network.peer(1).swarmState(1).connections.at(2).remoteInterestedInUs);
}

struct SchedulingFixture {
    Swarm swarm{1, InfoHash{1}, 200000, 100000};
    Swarm other{2, InfoHash{2}, 200000, 100000};
    Simulation simulation;
    Network network;
    SchedulingFixture() : network(simulation, joinedPeers(),
        {Link(1, 2, 1000000, .01), Link(2, 3, 1000000, .01)}, {swarm, other}) {}
    std::vector<Peer> joinedPeers() {
        std::vector<Peer> peers;
        for (PeerId id : {1u, 2u, 3u}) {
            peers.emplace_back(id, 2000000, 2000000, PeerProtocolId{static_cast<std::uint8_t>(id)});
            for (const auto* current : {&swarm, &other}) peers.back().joinSwarm(*current, {static_cast<std::uint8_t>(id == 2 ? 0 : 0xc0)});
        }
        for (const auto* current : {&swarm, &other}) {
            for (PeerId id : {1u, 3u}) {
                auto& local = peers[1];
                auto& remote = peers[id - 1];
                local.markHandshakeSent(*current, id);
                remote.markHandshakeSent(*current, 2);
                local.receiveMessage(*current, id, handshake(*current, remote.protocolId()), &remote.protocolId());
                remote.receiveMessage(*current, 2, handshake(*current, local.protocolId()), &local.protocolId());
                auto& incoming = const_cast<PeerSwarmState&>(remote.swarmState(current->id())).connections.at(2);
                incoming.remoteInterestedInUs = true;
                incoming.weAreChokingRemote = false;
                incoming.bitfieldSent = true;
            }
        }
        return peers;
    }
    const PeerConnectionState& connection(SwarmId id = 1, PeerId remote = 1) const {
        return network.peer(2).swarmState(id).connections.at(remote);
    }
};

struct RarityFixture {
    Swarm swarm{1, InfoHash{1}, 300000, 100000};
    Swarm other{2, InfoHash{2}, 300000, 100000};
    Simulation simulation;
    Network network;
    RarityFixture() : network(simulation, joinedPeers(),
        {Link(1, 3, 1e6, .01), Link(1, 4, 1e6, .01), Link(1, 5, 1e6, .01),
         Link(2, 3, 1e6, .01), Link(2, 4, 1e6, .01), Link(2, 5, 1e6, .01)}, {swarm, other}) {}
    std::vector<Peer> joinedPeers() {
        std::vector<Peer> peers;
        for (PeerId id = 1; id <= 5; ++id) {
            peers.emplace_back(id, 2e6, 2e6);
            for (const auto* s : {&swarm, &other}) peers.back().joinSwarm(*s, {static_cast<std::uint8_t>(id <= 2 ? 0 : 0xe0)});
        }
        for (const auto* s : {&swarm, &other}) for (PeerId local : {1u, 2u}) for (PeerId remote : {3u, 4u, 5u}) {
            auto& a = peers[local - 1]; auto& b = peers[remote - 1];
            a.markHandshakeSent(*s, remote); b.markHandshakeSent(*s, local);
            a.receiveMessage(*s, remote, handshake(*s, b.protocolId()), &b.protocolId());
            b.receiveMessage(*s, local, handshake(*s, a.protocolId()), &a.protocolId());
            auto& from = const_cast<PeerSwarmState&>(b.swarmState(s->id())).connections.at(local);
            from.weAreChokingRemote = false;
            from.bitfieldSent = true;
            const_cast<PeerSwarmState&>(a.swarmState(s->id())).connections.at(remote).bitfieldSent = true;
        }
        return peers;
    }
    PeerSwarmState& state(PeerId local = 1, SwarmId swarmId = 1) {
        return const_cast<PeerSwarmState&>(network.peer(local).swarmState(swarmId));
    }
    void advertise(PeerId remote, std::uint8_t bits, PeerId local = 1, SwarmId swarmId = 1) {
        network.deliver(swarmId, remote, local, Message(MessageType::Bitfield, BitfieldPayload{{bits}}));
    }
    void unchoke(PeerId remote = 3, PeerId local = 1, SwarmId swarmId = 1) {
        network.deliver(swarmId, remote, local, Message(MessageType::Unchoke));
    }
    const RequestPayload& first(PeerId local = 1, SwarmId swarmId = 1, PeerId remote = 3) {
        const auto& requests = state(local, swarmId).connections.at(remote).scheduledRequests;
        check(!requests.empty());
        return requests.front();
    }
};
void rarestOverLowerIndex() {
    RarityFixture f;
    f.advertise(3, 0xe0); f.advertise(4, 0xc0); f.advertise(5, 0x80);
    f.unchoke();
    check(f.first() == RequestPayload{2, 0, 16384}); // Counts: 3, 2, 1.
}
void rarityTieByIndex() {
    RarityFixture f;
    f.advertise(3, 0xe0); f.advertise(4, 0xe0);
    f.unchoke();
    check(f.first() == RequestPayload{0, 0, 16384});
}
void rarityIncludesChokedRemote() {
    RarityFixture f;
    f.advertise(3, 0xc0); f.advertise(4, 0x80);
    check(f.state().connections.at(4).remoteIsChokingUs);
    f.unchoke();
    check(f.first().index == 1);
}
void rarityUnknownIsNotGlobalInventory() {
    RarityFixture f;
    // Actual inventories differ from the requester's unadvertised, zero knowledge.
    f.state(4).localBitfield = {0x80}; f.state(5).localBitfield = {0x80};
    f.advertise(3, 0xc0);
    f.unchoke();
    check(f.first().index == 0); // Only peer 3 is known to offer either piece.
}
void rarityRequiresEstablishedConnection() {
    RarityFixture f;
    f.advertise(3, 0xc0);
    auto& unestablished = f.state().connections.at(4);
    unestablished.remoteBitfield = {0x80};
    unestablished.handshakeReceived = false;
    f.unchoke();
    check(f.first().index == 0);
}
void rarityRepeatedHaveIsIdempotent() {
    RarityFixture f;
    f.advertise(3, 0xc0); f.advertise(4, 0x80); f.advertise(5, 0x40);
    for (int i = 0; i < 5; ++i) f.network.deliver(1, 4, 1, Message(MessageType::Have, HavePayload{0}));
    f.unchoke();
    check(f.first().index == 0); // Two known copies each; repeated HAVE is not another copy.
}
void rarityBitfieldReplacement() {
    for (bool replace : {false, true}) {
        RarityFixture f;
        f.advertise(3, 0xc0); f.advertise(4, 0x80);
        if (replace) f.advertise(4, 0x40);
        f.unchoke();
        check(f.first().index == (replace ? 0u : 1u));
    }
}
void raritySkipsCompletedAndReserved() {
    for (bool outgoing : {false, true}) {
        RarityFixture f;
        f.state().localBitfield = {0x80};
        f.state().receivedBlocks[2] = {{0, 1024}, {2048, 4096}};
        auto& remote = f.state().connections.at(4);
        (outgoing ? remote.outgoingRequests : remote.scheduledRequests).push_back({1, 0, 100000});
        f.advertise(3, 0xe0);
        f.unchoke();
        check(f.first() == RequestPayload{2, 1024, 1024}); // Preserve gap-sized block selection.
    }
}
void rarityTargetEligibility() {
    RarityFixture f;
    f.advertise(3, 0xc0); f.advertise(4, 0x80); f.advertise(5, 0x80);
    f.state(3).localBitfield = {0x80}; // Target cannot actually supply the advertised rarer piece 1.
    f.unchoke();
    check(f.first().index == 0);
    RarityFixture unknown;
    unknown.advertise(3, 0x80); // Actual target owns more, but those pieces are not advertised.
    unknown.unchoke();
    check(unknown.first().index == 0);
}
void raritySimultaneousUnchokes() {
    RarityFixture f;
    f.advertise(3, 0xe0); f.advertise(4, 0xe0); f.advertise(5, 0xc0);
    std::vector<RequestPayload> started;
    bool usedThree = false, usedFour = false;
    f.simulation.setEventObserver([&](const Event& event) {
        check(event.time() < 100); // Completion must drain the policy timers too.
        std::vector<RequestPayload> reserved;
        for (const auto& [id, c] : f.state().connections) {
            check(c.scheduledRequests.size() + c.outgoingRequests.size() <= 5);
            for (const auto* requests : {&c.scheduledRequests, &c.outgoingRequests}) for (const auto& block : *requests) {
                for (const auto& earlier : reserved) if (earlier.index == block.index)
                    check(block.begin >= earlier.begin + earlier.length || earlier.begin >= block.begin + block.length);
                reserved.push_back(block);
            }
        }
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
            start && start->details().sender == 1 && start->details().messageType == MessageType::Request) {
            const auto block = std::get<RequestPayload>(start->message().payload());
            if (started.empty()) check(block.index == 2);
            for (const auto& earlier : started) if (earlier.index == block.index)
                check(block.begin >= earlier.begin + earlier.length || earlier.begin >= block.begin + block.length);
            started.push_back(block);
            usedThree |= start->details().receiver == 3;
            usedFour |= start->details().receiver == 4;
        }
    });
    for (PeerId remote : {3u, 4u}) f.simulation.schedule(std::make_unique<SendMessageEvent>(
        0, f.network, 1, remote, 1, Message(MessageType::Unchoke)));
    f.simulation.run();
    check(usedThree && usedFour && f.state().localBitfield[0] == 0xe0);
    for (const auto& [id, c] : f.state().connections) check(c.scheduledRequests.empty() && c.outgoingRequests.empty());
}
void rarityPerPeerIsolation() {
    RarityFixture f;
    f.advertise(3, 0xc0, 1); f.advertise(4, 0x80, 1);
    f.advertise(3, 0xc0, 2); f.advertise(4, 0x40, 2);
    f.unchoke(3, 1); f.unchoke(3, 2);
    check(f.first(1).index == 1 && f.first(2).index == 0);
}
void rarityPerSwarmIsolation() {
    RarityFixture f;
    f.advertise(3, 0xc0, 1, 1); f.advertise(4, 0x80, 1, 1);
    f.advertise(3, 0xc0, 1, 2); f.advertise(4, 0x40, 1, 2);
    f.unchoke(3, 1, 1); f.unchoke(3, 1, 2);
    check(f.first(1, 1).index == 1 && f.first(1, 2).index == 0);
}
void completionReevaluatesInterestAcrossConnections()
{
    SchedulingFixture f;
    auto& state = const_cast<PeerSwarmState&>(f.network.peer(2).swarmState(1));
    // Peer 3 is useful only for piece 0; it stays choked, so peer 1 supplies the data.
    state.connections.at(3).remoteBitfield[0] = 0x80;
    state.connections.at(3).weAreInterestedInRemote = true;
    auto& other = const_cast<PeerSwarmState&>(f.network.peer(2).swarmState(2));
    other.connections.at(1).remoteBitfield[0] = 0xc0;
    other.connections.at(1).weAreInterestedInRemote = true;
    const auto otherBefore = other;
    std::vector<ExecutedMessage> events;
    unsigned notInterestedToOne = 0, notInterestedToThree = 0;
    bool usefulAfterRarerPiece = false;
    f.simulation.setEventObserver([&](const Event& event) {
        if (const auto* send = dynamic_cast<const SendMessageEvent*>(&event);
            send && send->details().messageType == MessageType::Have && state.localBitfield[0] == 0x40) {
            // Piece 1 is rarer and completes first; both remotes still offer piece 0.
            check(f.connection().weAreInterestedInRemote && f.connection(1, 3).weAreInterestedInRemote);
            usefulAfterRarerPiece = true;
        }
        if (const auto* send = dynamic_cast<const SendMessageEvent*>(&event);
            send && send->details().messageType == MessageType::NotInterested) {
            const auto details = send->details();
            check(details.sender == 2 && details.swarmId == 1);
            check(!f.connection(1, details.receiver).weAreInterestedInRemote);
            // Remote state changes only after the FIFO transmission and propagation.
            check(f.network.peer(details.receiver).swarmState(1).connections.at(2).remoteInterestedInUs);
            if (details.receiver == 3) {
                ++notInterestedToThree;
                check(state.localBitfield[0] == 0xc0);
                check(!f.connection().weAreInterestedInRemote); // Both advertised pieces are complete.
            } else {
                check(details.receiver == 1);
                ++notInterestedToOne;
                check(state.localBitfield[0] == 0xc0);
            }
            events.push_back({ExecutedKind::Request, event.time(), details});
        }
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
            start && start->details().messageType == MessageType::NotInterested) {
            events.push_back({ExecutedKind::Start, event.time(), start->details()});
        }
        if (const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event);
            complete && complete->details()->messageType == MessageType::NotInterested) {
            events.push_back({ExecutedKind::Complete, event.time(), *complete->details()});
        }
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event);
            arrival && arrival->details().messageType == MessageType::NotInterested) {
            events.push_back({ExecutedKind::Arrival, event.time(), arrival->details()});
            check(f.network.peer(arrival->details().receiver).swarmState(1).connections.at(2).remoteInterestedInUs);
        }
    });
    f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0xc0}}));
    f.network.deliver(1, 1, 2, Message(MessageType::Unchoke));
    f.simulation.run();
    check(usefulAfterRarerPiece && notInterestedToOne == 1 && notInterestedToThree == 1);
    for (PeerId remote : {1u, 3u}) {
        std::vector<ExecutedMessage> flow;
        for (const auto& event : events) if (event.details.receiver == remote) flow.push_back(event);
        check(flow.size() == 4);
        check(flow[0].kind == ExecutedKind::Request && flow[1].kind == ExecutedKind::Start);
        check(flow[2].kind == ExecutedKind::Complete && flow[3].kind == ExecutedKind::Arrival);
        check(flow[1].time > flow[0].time); // The completion HAVE is ahead in this direction's FIFO.
        check(std::abs(flow[2].time - flow[1].time - 5 * 8.0 / 1000000) < 1e-12);
        check(std::abs(flow[3].time - flow[2].time - .01) < 1e-12);
        check(!f.connection(1, remote).weAreInterestedInRemote);
        check(!f.network.peer(remote).swarmState(1).connections.at(2).remoteInterestedInUs);
    }
    checkUnchanged(other, otherBefore);
    // Repeated availability for already-owned pieces must not repeat NOT_INTERESTED.
    f.network.deliver(1, 1, 2, Message(MessageType::Have, HavePayload{1}));
    f.network.deliver(1, 3, 2, Message(MessageType::Have, HavePayload{0}));
    f.simulation.run();
    check(usefulAfterRarerPiece && notInterestedToOne == 1 && notInterestedToThree == 1);
}

void schedulerGatesReservationsAndResume()
{
    SchedulingFixture f;
    std::vector<ExecutedMessage> events;
    observeMessages(f.simulation, events);
    f.network.deliver(1, 1, 2, Message(MessageType::Unchoke)); // No advertised pieces / interest.
    check(f.connection().scheduledRequests.empty());
    f.network.deliver(1, 1, 2, Message(MessageType::Choke));
    f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0xc0}}));
    check(f.connection().weAreInterestedInRemote && f.connection().scheduledRequests.empty());
    f.network.deliver(1, 1, 2, Message(MessageType::Unchoke));
    check(f.connection().scheduledRequests.size() == 5 && f.connection().outgoingRequests.empty());
    for (int i = 0; i < 3; ++i) {
        f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0xc0}}));
        f.network.deliver(1, 1, 2, Message(MessageType::Have, HavePayload{0}));
        f.network.deliver(1, 1, 2, Message(MessageType::Unchoke));
    }
    check(f.connection().scheduledRequests.size() == 5);
    // Choke arrives before the reserved send events execute; reservations must be released.
    f.network.deliver(1, 1, 2, Message(MessageType::Choke));
    // This fixture deliberately injects a receive-side CHOKE without changing
    // the sender. Inspect the reservation behavior before the periodic policy tick.
    struct Pause {};
    f.simulation.schedule(std::make_unique<CheckEvent>(.1, [] { throw Pause{}; }));
    try { f.simulation.run(); } catch (const Pause&) {}
    check(f.connection().scheduledRequests.empty() && f.connection().outgoingRequests.empty());
    check(std::none_of(events.begin(), events.end(), [](const auto& event) {
        return event.kind == ExecutedKind::Start && event.details.messageType == MessageType::Request;
    }));
    f.network.deliver(1, 1, 2, Message(MessageType::Unchoke));
    check(f.connection().scheduledRequests.size() == 5);
    f.simulation.run();
    check(f.network.peer(2).swarmState(1).localBitfield[0] == 0xc0);
    check(f.connection().scheduledRequests.empty() && f.connection().outgoingRequests.empty());
    check(f.network.peer(2).swarmState(2).localBitfield[0] == 0);
}

void schedulerSkipsReceivedAndOutstandingAcrossPeers()
{
    SchedulingFixture f;
    auto& state = const_cast<PeerSwarmState&>(f.network.peer(2).swarmState(1));
    state.receivedBlocks[0] = {{0, 1024}, {2048, 4096}};
    state.connections.at(3).weAreInterestedInRemote = true;
    state.connections.at(3).remoteIsChokingUs = false;
    const RequestPayload explicitBlock{0, 8192, 1024};
    f.network.send(1, 2, 3, Message(MessageType::Request, explicitBlock));
    f.network.deliver(1, 1, 2, Message(MessageType::Unchoke));
    f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0xc0}}));
    const auto& reserved = f.connection().scheduledRequests;
    check(reserved.size() == 5);
    check(reserved[0] == RequestPayload{0, 1024, 1024});
    check(reserved[1] == RequestPayload{0, 4096, 4096});
    check(reserved[2] == RequestPayload{0, 9216, 16384});
    f.network.deliver(1, 3, 2, Message(MessageType::Bitfield, BitfieldPayload{{0xc0}}));
    check(f.connection(1, 3).scheduledRequests.size() == 4); // One manual request already consumes capacity.
    f.network.deliver(2, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0xc0}}));
    f.network.deliver(2, 1, 2, Message(MessageType::Unchoke));
    check(f.connection(2).scheduledRequests.front() == RequestPayload{0, 0, 16384});
    std::vector<RequestPayload> requested{explicitBlock};
    f.simulation.setEventObserver([&](const Event& event) {
        const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
        if (!start || start->details().messageType != MessageType::Request || start->details().sender != 2) return;
        const auto& block = std::get<RequestPayload>(start->message().payload());
        if (start->details().swarmId != 1) return;
        for (const auto& earlier : requested) {
            if (earlier.index == block.index) check(block.begin >= earlier.begin + earlier.length || earlier.begin >= block.begin + block.length);
        }
        if (block.index == 0) {
            check(block.begin >= 1024);
            check(block.begin >= 4096 || block.begin + block.length <= 2048);
        }
        requested.push_back(block);
        for (const auto& [remote, connection] : f.network.peer(2).swarmState(1).connections) {
            check(connection.outgoingRequests.size() + connection.scheduledRequests.size() <= 5);
        }
    });
    f.simulation.run();
    for (SwarmId id : {1u, 2u}) {
        const auto& complete = f.network.peer(2).swarmState(id);
        check(complete.localBitfield[0] == 0xc0);
        for (PeerId remote : {1u, 3u}) {
            check(complete.connections.at(remote).outgoingRequests.empty());
            check(complete.connections.at(remote).scheduledRequests.empty());
        }
    }
}

void schedulerReplansWithdrawnAvailability()
{
    SchedulingFixture f;
    f.network.deliver(1, 1, 2, Message(MessageType::Unchoke));
    f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0xc0}}));
    check(f.connection().scheduledRequests.size() == 5);
    // Withdraw piece 0 before send events execute; piece 1 must still download.
    f.network.deliver(1, 1, 2, Message(MessageType::Bitfield, BitfieldPayload{{0x40}}));
    std::vector<RequestPayload> requests;
    f.simulation.setEventObserver([&](const Event& event) {
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
            start && start->details().messageType == MessageType::Request) {
            requests.push_back(std::get<RequestPayload>(start->message().payload()));
        }
    });
    f.simulation.run();
    check(requests.size() == 7);
    check(std::all_of(requests.begin(), requests.end(), [](const auto& block) { return block.index == 1; }));
    check(f.network.peer(2).swarmState(1).localBitfield[0] == 0x40);
    // Finishing the advertised piece sent NOT_INTERESTED and caused CHOKE.
    // New availability must restore interest, then wait for UNCHOKE before requesting.
    check(!f.connection().weAreInterestedInRemote && f.connection().remoteIsChokingUs);
    f.network.deliver(1, 1, 2, Message(MessageType::Have, HavePayload{0}));
    check(f.connection().weAreInterestedInRemote && f.connection().scheduledRequests.empty());
    f.simulation.run();
    check(f.network.peer(2).swarmState(1).localBitfield[0] == 0xc0);
}

void twoWayHandshakeLifecycle()
{
    HandshakeFixture f;
    const PeerConnectionState initial;
    check(!initial.handshakeSent && !initial.handshakeReceived && !initial.handshakeComplete());
    const std::vector<Message> ordinary{
        Message(MessageType::Choke), Message(MessageType::Unchoke),
        Message(MessageType::Interested), Message(MessageType::NotInterested),
        Message(MessageType::Have, HavePayload{8}),
        Message(MessageType::Bitfield, BitfieldPayload{{0x01, 0x80}})
    };

    // A received a valid handshake but has not sent one: every ordinary type rejects.
    Peer receivedOnly = f.receiver;
    receivedOnly.receiveMessage(f.swarm, 1, handshake(f.swarm), &testSenderProtocolId);
    const auto receivedBefore = receivedOnly.swarmState(1);
    check(receivedBefore.connections.at(1).handshakeReceived);
    check(!receivedBefore.connections.at(1).handshakeSent);
    check(!receivedBefore.connections.at(1).handshakeComplete());
    for (const auto& message : ordinary) {
        rejects([&] { receivedOnly.receiveMessage(f.swarm, 1, message); });
        checkUnchanged(receivedOnly.swarmState(1), receivedBefore);
    }

    f.simulation.schedule(std::make_unique<SendMessageEvent>(
        0.0, f.network, 1, 1, 2, handshake(f.swarm)));
    check(f.network.peer(1).swarmState(1).connections.empty());
    f.simulation.schedule(std::make_unique<CheckEvent>(0.0, [&] {
        const auto before = f.network.peer(1).swarmState(1);
        const auto& a = before.connections.at(2);
        check(a.handshakeSent && !a.handshakeReceived && !a.handshakeComplete());
        check(f.state().connections.empty());
        for (const auto& message : ordinary) {
            rejects([&] { f.network.deliver(1, 2, 1, message); });
            checkUnchanged(f.network.peer(1).swarmState(1), before);
        }
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(1.189, [&] {
        const auto& b = f.state().connections.at(1);
        check(b.handshakeReceived && b.handshakeSent && b.handshakeComplete());
        check(!f.network.peer(1).swarmState(1).connections.at(2).handshakeReceived);
        const auto before = f.state();
        // Two duplicate arrivals must not schedule additional responses.
        f.deliver(handshake(f.swarm));
        f.deliver(handshake(f.swarm));
        checkUnchanged(f.state(), before);
    }));
    f.simulation.run();
    check(f.network.peer(1).swarmState(1).connections.at(2).handshakeComplete());
    check(f.state().connections.at(1).handshakeComplete());
    // Handshake response arrives at 2.376; A's BITFIELD arrives 0.212 later.
    check(std::abs(f.simulation.currentTime() - 2.588) < 1e-12);
    for (const auto& message : ordinary) {
        f.network.deliver(1, 1, 2, message);
        f.network.deliver(1, 2, 1, message);
    }
    f.simulation.run(); // Drain interest transitions caused by the ordinary messages above.
    const double finished = f.simulation.currentTime();
    f.deliver(handshake(f.swarm));
    f.simulation.run();
    check(f.simulation.currentTime() == finished);
}

void failedHandshakeSend()
{
    HandshakeFixture f;
    rejects([&] { f.network.send(1, 1, 3, handshake(f.swarm)); });
    check(f.network.peer(1).swarmState(1).connections.empty());
}

void validHandshake()
{
    HandshakeFixture f;
    check(!PeerConnectionState{}.handshakeComplete());
    check(f.sender.id() == 1 && f.sender.protocolId() == testSenderProtocolId);
    check(f.state().connections.empty());
    f.complete();
    const auto& remote = f.state().connections.at(1);
    check(remote.handshakeComplete());
    check(remote.remoteIsChokingUs && !remote.remoteInterestedInUs);
    check(remote.remoteBitfield == std::vector<std::uint8_t>({0, 0}));
    f.deliver(Message(MessageType::Unchoke));
    check(!f.state().connections.at(1).remoteIsChokingUs);
    const auto before = f.state();
    f.complete();
    checkUnchanged(f.state(), before);
}

void invalidHandshake(bool wrongHash)
{
    HandshakeFixture f;
    f.network.deliver(1, 3, 2, handshake(f.swarm, f.third.protocolId()));
    f.network.deliver(1, 3, 2, Message(MessageType::Interested));
    f.network.deliver(2, 1, 2, handshake(f.other, f.sender.protocolId()));
    const auto otherBefore = f.network.peer(2).swarmState(2);
    // Every byte, including the last byte, must participate in equality.
    for (std::size_t byte = 0; byte < 20; ++byte) {
        auto payload = HandshakePayload{f.swarm.infoHash(), f.sender.protocolId()};
        if (wrongHash) payload.infoHash[byte] ^= 1;
        else payload.peerId[byte] ^= 1;
        const auto before = f.state();
        rejects([&] { f.deliver(Message(MessageType::Handshake, payload)); });
        checkUnchanged(f.state(), before);
        check(!f.state().connections.contains(1));
        checkUnchanged(f.network.peer(2).swarmState(2), otherBefore);
    }
    // The existing CANCEL no-op creates a default connection, allowing us
    // to check an existing incomplete connection without adding protocol behavior.
    f.deliver(Message(MessageType::Cancel, CancelPayload{}));
    check(!f.state().connections.at(1).handshakeComplete());
    auto payload = HandshakePayload{f.swarm.infoHash(), f.sender.protocolId()};
    if (wrongHash) payload.infoHash.back() ^= 1;
    else payload.peerId.back() ^= 1;
    const auto before = f.state();
    rejects([&] { f.deliver(Message(MessageType::Handshake, payload)); });
    checkUnchanged(f.state(), before);
    check(!f.state().connections.at(1).handshakeComplete());
    f.complete();
    const auto completeBefore = f.state();
    rejects([&] { f.deliver(Message(MessageType::Handshake, payload)); });
    checkUnchanged(f.state(), completeBefore);
}

void ordinaryMessagesRequireHandshake()
{
    HandshakeFixture f;
    const std::vector<Message> messages{
        Message(MessageType::Choke), Message(MessageType::Unchoke),
        Message(MessageType::Interested), Message(MessageType::NotInterested),
        Message(MessageType::Have, HavePayload{8}),
        Message(MessageType::Bitfield, BitfieldPayload{{0x01, 0x80}})
    };
    for (bool existingConnection : {false, true}) {
        if (existingConnection) f.deliver(Message(MessageType::Cancel, CancelPayload{}));
        const auto before = f.state();
        for (const auto& message : messages) {
            rejects([&] { f.deliver(message); });
            checkUnchanged(f.state(), before);
        }
    }
    f.complete();
    for (const auto& message : messages) f.deliver(message);
    check(f.state().connections.at(1).handshakeComplete());
    check(!f.state().connections.at(1).remoteIsChokingUs);
    check(!f.state().connections.at(1).remoteInterestedInUs);
    check(f.state().connections.at(1).remoteBitfield == std::vector<std::uint8_t>({0x01, 0x80}));
}

void handshakeScope()
{
    HandshakeFixture f;
    f.complete();
    const auto before = f.state();
    rejects([&] { f.network.deliver(1, 3, 2, Message(MessageType::Unchoke)); });
    rejects([&] { f.network.deliver(2, 1, 2, Message(MessageType::Unchoke)); });
    f.network.deliver(1, 2, 1, Message(MessageType::Unchoke));
    checkUnchanged(f.state(), before);
    check(f.network.peer(2).swarmState(2).connections.empty());
    check(f.network.peer(1).swarmState(1).connections.at(2).handshakeComplete());
}

void handshakeUnjoinedSwarm()
{
    const Swarm swarm(1, InfoHash{0x77}, 9);
    Peer sender(1, 1000, 1000, testSenderProtocolId);
    Peer receiver(2);
    Simulation simulation;
    Network network(simulation, {sender, receiver}, {}, {swarm});
    rejects([&] { network.deliver(1, 1, 2, handshake(swarm)); });
    check(!network.peer(2).hasSwarm(1));
}

void handshakeRequiresSenderContext()
{
    Fixture f(false);
    const auto before = f.receiver.swarmState(1);
    rejects([&] { f.receive(handshake(f.swarm)); });
    checkUnchanged(f.receiver.swarmState(1), before);
    HandshakeFixture n;
    rejects([&] { n.network.deliver(1, 99, 2, handshake(n.swarm)); });
    check(n.state().connections.empty());
}

class ObserveHandshakeEvent : public Event {
public:
    ObserveHandshakeEvent(double time, const Network& network, bool expected, int& observations)
        : Event(time), network_(network), expected_(expected), observations_(observations) {}
    void execute() override {
        const auto& connections = network_.peer(2).swarmState(1).connections;
        if (expected_) check(connections.at(1).handshakeComplete());
        else check(connections.empty());
        ++observations_;
    }
private:
    const Network& network_;
    bool expected_;
    int& observations_;
};

void handshakeArrivalTiming()
{
    HandshakeFixture f;
    int observations = 0;
    check(handshake(f.swarm).wireSize() == 68);
    f.simulation.schedule(std::make_unique<SendMessageEvent>(
        0.0, f.network, 1, 1, 2, handshake(f.swarm)));
    // Arrival is 0.1 + 68 * 8 / 500 = 1.188 simulation seconds.
    f.simulation.schedule(std::make_unique<ObserveHandshakeEvent>(0.0, f.network, false, observations));
    f.simulation.schedule(std::make_unique<ObserveHandshakeEvent>(1.187, f.network, false, observations));
    f.simulation.schedule(std::make_unique<ObserveHandshakeEvent>(1.189, f.network, true, observations));
    f.simulation.schedule(std::make_unique<SendMessageEvent>(
        2.0, f.network, 1, 1, 2, Message(MessageType::Unchoke)));
    check(f.state().connections.empty());
    f.simulation.run();
    check(observations == 3);
    check(f.state().connections.at(1).handshakeComplete());
    check(!f.state().connections.at(1).remoteIsChokingUs);
    check(std::abs(f.simulation.currentTime() - 2.588) < 1e-12);
}
} // namespace

int main()
{
    std::cout << std::unitbuf;
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"Explicit deterministic 6/4/4 Mbps sharing and release", equalShareExplicitSixFourFour},
        {"Equal share upload", equalShareUpload},
        {"Equal share upload release and progress", equalShareUploadRelease},
        {"Equal share download", equalShareDownload},
        {"Equal share both endpoint limits", equalShareEndpointLimits},
        {"Equal share link cap avoids redundant scheduling", equalShareLinkCapUnchanged},
        {"Equal share unrelated flow untouched", equalShareUnrelated},
        {"Equal share FIFO membership", equalShareFifo},
        {"Equal share full duplex", equalShareDuplex},
        {"Equal share deterministic simultaneous starts", equalShareSameTimestamp},
        {"Equal share aggregate invariants at transitions", equalShareAggregateTransitions},
        {"Equal share old-rate progress before start", equalShareStartProgress},
        {"Mbps slowdown progress and arrival", mbpsSlowdown},
        {"Mbps speedup earlier completion", mbpsSpeedup},
        {"Mbps multiple interval progress", mbpsMultipleChanges},
        {"Mbps rate change at start timestamp", mbpsSameStartTime},
        {"Mbps rate change one ULP before completion", mbpsNearCompletion},
        {"Mbps every stale completion is a no-op", mbpsStaleSafety},
        {"Mbps FIFO released once by final generation", mbpsFifo},
        {"Mbps full-duplex independence", mbpsFullDuplex},
        {"Mbps independent link unaffected", mbpsIndependentLink},
        {"Mbps completion and arrival uniqueness", mbpsUniqueness},
        {"Dynamic rate math, stale completion and FIFO", dynamicTransmissionRateMath},
        {"Repeated transmission rate changes", repeatedTransmissionRateChanges},
        {"Rate validation and exact completion boundary", transmissionRateValidationAndBoundary},
        {"Active transmission lifecycle", activeTransmissionLifecycle},
        {"Arrival scheduled only by valid completion", arrivalScheduledByCompletion},
        {"Stale completion has no transport or protocol effects", staleCompletionNoOp},
        {"Rarest piece beats lower-index common pieces", rarestOverLowerIndex},
        {"Equal rarity breaks ties by piece index", rarityTieByIndex},
        {"Choked known remote contributes to rarity", rarityIncludesChokedRemote},
        {"Unknown availability does not reveal actual inventories", rarityUnknownIsNotGlobalInventory},
        {"Unestablished connection does not contribute to rarity", rarityRequiresEstablishedConnection},
        {"Repeated HAVE does not inflate rarity", rarityRepeatedHaveIsIdempotent},
        {"BITFIELD replacement updates rarity", rarityBitfieldReplacement},
        {"Rarity skips completed and reserved pieces and preserves gaps", raritySkipsCompletedAndReserved},
        {"Rarity retains target ownership and advertisement safeguards", rarityTargetEligibility},
        {"Simultaneous unchokes reserve non-overlapping rarest-first blocks", raritySimultaneousUnchokes},
        {"Rarity knowledge is isolated per peer", rarityPerPeerIsolation},
        {"Rarity knowledge is isolated per swarm", rarityPerSwarmIsolation},
        {"Piece completion reevaluates all swarm connections through FIFO", completionReevaluatesInterestAcrossConnections},
        {"Autonomous sequential pipeline, refill and short blocks", autonomousSequentialPipeline},
        {"Scheduler gates, duplicate reservations and unchoke resume", schedulerGatesReservationsAndResume},
        {"Scheduler excludes received/outstanding ranges across peers and swarms", schedulerSkipsReceivedAndOutstandingAcrossPeers},
        {"Scheduler replans withdrawn availability and resumes on HAVE", schedulerReplansWithdrawnAvailability},
        {"Completion HAVE scope, FIFO and receive-side interest", completionHaveScopeFifoAndInterest},
        {"Completion HAVE only on first full coverage", completionHaveOnlyOnFirstCompletion},
        {"PIECE requires established sender context", pieceRequiresEstablishedSenderContext},
        {"In-flight PIECE remains valid after choking", pieceInFlightSurvivesChoking},
        {"Queued PIECE revalidates outstanding request", queuedPieceRevalidatesOutstandingRequest},
        {"PIECE payload-sized timing and FIFO arrival", piecePayloadTimingAndFifo},
        {"Overlapping blocks, gaps and piece completion", overlappingBlocksAndCompletion},
        {"Sixteen blocks and shorter final piece", sixteenBlocksAndShortFinalPiece},
        {"Unsolicited, wrong-peer, duplicate and invalid PIECE", rejectedPiecePreservesState},
        {"PIECE swarm isolation and blocks from multiple peers", pieceSwarmAndRemoteScope},
        {"Queued PIECE revalidates choking at start", pieceRevalidatesAtTransmissionStart},
        {"Swarm sizes and shortened final piece", swarmPieceSizes},
        {"Valid REQUEST and duplicate pending/accepted suppression", validRequestAndDuplicates},
        {"Invalid REQUEST ranges preserve state", invalidRequestRanges},
        {"REQUEST connection and ownership validation", requestConnectionAndOwnershipValidation},
        {"REQUEST scope and distinct blocks", requestScopeAndDistinctBlocks},
        {"REQUEST accepted only on FIFO arrival", requestFifoArrival},
        {"REQUEST revalidated on arrival", requestArrivalRevalidation},
        {"Choke policy, duplicate suppression and arrival-only updates", chokePolicyAndArrival},
        {"Choke policy directional, swarm and remote-peer scope", chokePolicyScope},
        {"CHOKE and UNCHOKE wait in directional FIFO", chokePolicyUsesFifo},
        {"BITFIELD interest transitions and duplicate suppression", bitfieldInterestTransitions},
        {"HAVE interest transitions and duplicate suppression", haveInterestTransitions},
        {"Interest scope by swarm and remote peer", interestScope},
        {"Interest messages wait in directional FIFO", interestUsesDirectionalFifo},
        {"Invalid availability preserves interest and sends nothing", invalidAvailabilityPreservesInterest},
        {"Validated initial local bitfield", initialLocalBitfield},
        {"Exact request/start/completion/arrival event times", exactTransmissionEventTimes},
        {"FIFO start events for a 31-message burst", fifoTransmissionStartEvents},
        {"Simultaneous full-duplex start events", simultaneousTransmissionStartEvents},
        {"Automatic handshake and BITFIELD use all four events", automaticProtocolEventFlow},
        {"Automatic BITFIELD exchange, FIFO timing and duplicates", automaticBitfieldExchange},
        {"Automatic BITFIELD scope by swarm and remote peer", automaticBitfieldScope},
        {"Receiving BITFIELD never triggers a response", bitfieldDoesNotTriggerResponse},
        {"Simultaneous handshakes exchange BITFIELDs once", simultaneousHandshakeBitfields},
        {"Same-direction serialization and propagation latency", serializedTransmissionAndLatency},
        {"Full-duplex simultaneous transmission", fullDuplexTransmission},
        {"FIFO burst of 31 reverse-direction messages", fifoBurst},
        {"Enqueue at completion with zero latency", enqueueAtCompletion},
        {"Queued handshake response starts once", queuedHandshakeResponse},
        {"Two-way handshake lifecycle and duplicate responses", twoWayHandshakeLifecycle},
        {"Failed handshake send leaves state unchanged", failedHandshakeSend},
        {"Valid handshake and subsequent UNCHOKE", validHandshake},
        {"Wrong handshake infoHash", [] { invalidHandshake(true); }},
        {"Wrong handshake PeerProtocolId", [] { invalidHandshake(false); }},
        {"All ordinary messages require handshake", ordinaryMessagesRequireHandshake},
        {"Handshake scope: receiver, swarm, remote", handshakeScope},
        {"Handshake for unjoined swarm", handshakeUnjoinedSwarm},
        {"Handshake requires actual sender context", handshakeRequiresSenderContext},
        {"Handshake end-to-end arrival timing", handshakeArrivalTiming},
        {"UNCHOKE", unchoke}, {"CHOKE", choke},
        {"INTERESTED", interested}, {"NOT_INTERESTED", notInterested},
        {"HAVE bit positions and preservation", have},
        {"Invalid HAVE preserves state", invalidHave},
        {"BITFIELD exact replacement", bitfield},
        {"Invalid BITFIELD length preserves state", invalidBitfieldLength},
        {"Invalid BITFIELD trailing bits preserve state", invalidBitfieldTrailingBits},
        {"Unjoined swarm rejection", unjoinedSwarm},
        {"End-to-end arrival timing", arrivalTiming},
        {"Existing delivery regression", [] { check(existingDeliveryRegression() == 0); }}
    };
    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS: " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << "Tests: " << std::size(tests) << ", failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
