#include <cmath>
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

void check(bool condition)
{
    if (!condition) throw std::runtime_error("Message delivery check failed");
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

    explicit QueueFixture(double latency = 1.0)
        : network(simulation, joinedPeers(), {Link(1, 2, 800, latency)}, {swarm}) {}
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
    check(std::abs(f.simulation.currentTime() - 2.948) < 1e-12);

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
    check(std::abs(f.simulation.currentTime() - 1.76) < 1e-12);
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
    check(PeerConnectionState{}.weAreChokingRemote);
    check(PeerConnectionState{}.remoteIsChokingUs);
    f.receive(Message(MessageType::NotInterested)); // Initial no-op.
    f.simulation.run();
    check(f.events.empty());
    f.receive(Message(MessageType::Interested));
    f.receive(Message(MessageType::Interested));
    check(!f.local().weAreChokingRemote);
    check(f.local().remoteIsChokingUs); // Other direction remains choked.
    check(f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    f.simulation.schedule(std::make_unique<CheckEvent>(0.081, [&] {
        // Transmission completed at .08, but arrival is .18.
        check(f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    }));
    f.simulation.run();
    checkEventTimes(f.events, 2, MessageType::Unchoke, 0, 0, .08, .18);
    check(!f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    check(f.requests(MessageType::Unchoke) == 1);
    f.receive(Message(MessageType::Interested));
    f.simulation.run();
    check(f.requests(MessageType::Unchoke) == 1);

    f.receive(Message(MessageType::NotInterested));
    f.receive(Message(MessageType::NotInterested));
    check(f.local().weAreChokingRemote);
    check(!f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    f.simulation.schedule(std::make_unique<CheckEvent>(.261, [&] {
        check(!f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    }));
    f.simulation.run();
    checkEventTimes(f.events, 2, MessageType::Choke, .18, .18, .26, .36);
    check(f.requests(MessageType::Choke) == 1);
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
    occupyDirection(f.network, 1, 2, 1);
    f.receive(Message(MessageType::Interested));
    f.receive(Message(MessageType::NotInterested));
    f.receive(Message(MessageType::NotInterested));
    f.simulation.schedule(std::make_unique<CheckEvent>(.001, [&] {
        check(f.network.transmissionState(2, 1).pendingCount == 9);
        check(f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(2.357, [&] {
        check(!f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
    }));
    f.simulation.run();
    checkEventTimes(f.events, 2, MessageType::Unchoke, 0, 2.176, 2.256, 2.356);
    checkEventTimes(f.events, 2, MessageType::Choke, 0, 2.256, 2.336, 2.436);
    check(f.network.peer(1).swarmState(1).connections.at(2).remoteIsChokingUs);
}
struct RequestFixture {
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
        for (const auto* current : {&swarm, &other}) {
            for (PeerId remote : {1u, 3u}) {
                network.send(current->id(), 2, remote, handshake(*current, b.protocolId()));
            }
        }
        simulation.run();
        observeMessages(simulation, events);
    }
    std::vector<Peer> joinedPeers(bool ownsFirst, bool ownsSecond) {
        for (Peer* peer : {&a, &b, &c}) {
            peer->joinSwarm(swarm, {static_cast<std::uint8_t>(peer == &b ? 0x40 : (0x20 | (ownsFirst ? 0x80 : 0) | (ownsSecond ? 0x40 : 0)))});
            peer->joinSwarm(other, {static_cast<std::uint8_t>(peer == &b ? 0x40 : 0x80)});
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
        f.network.deliver(1, 2, 1, Message(MessageType::NotInterested));
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
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
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
