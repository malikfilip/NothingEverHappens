#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "simulator/Network.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"

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
        check(remote().remoteChokingUs && !remote().remoteInterestedInUs);
        check(remote().remoteBitfield == std::vector<std::uint8_t>({0, 0}));
        receive(Message(MessageType::Unchoke));
        check(!remote().remoteChokingUs);
        receive(Message(MessageType::Choke));
        check(remote().remoteChokingUs);
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
                Message(MessageType::Request, RequestPayload{}),
                Message(MessageType::Piece, PiecePayload{}),
                Message(MessageType::Cancel, CancelPayload{})}) {
            receive(message);
        }
        check(remote().remoteChokingUs && !remote().remoteInterestedInUs);
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
        check(network.peer(2).swarmState(1).connections.at(1).remoteChokingUs);
        simulation.run();
        check(!network.peer(2).swarmState(1).connections.at(1).remoteChokingUs);
        check(std::abs(simulation.currentTime() - 2.18) < 1e-12);
        check(network.peer(2).swarmState(2).connections.at(1).remoteChokingUs);
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
    check(actual.connections.size() == before.connections.size());
    for (const auto& [id, expected] : before.connections) {
        const auto& connection = actual.connections.at(id);
        check(connection.handshakeSent == expected.handshakeSent);
        check(connection.handshakeReceived == expected.handshakeReceived);
        check(connection.bitfieldSent == expected.bitfieldSent);
        check(connection.handshakeComplete() == expected.handshakeComplete());
        check(connection.remoteChokingUs == expected.remoteChokingUs);
        check(connection.remoteInterestedInUs == expected.remoteInterestedInUs);
        check(connection.remoteBitfield == expected.remoteBitfield);
    }
}

void unchoke()
{
    Fixture f;
    f.receive(Message(MessageType::Choke));
    check(f.remote().remoteChokingUs);
    f.receive(Message(MessageType::Unchoke));
    check(!f.remote().remoteChokingUs);
}

void choke()
{
    Fixture f;
    f.receive(Message(MessageType::Unchoke));
    check(!f.remote().remoteChokingUs);
    f.receive(Message(MessageType::Choke));
    check(f.remote().remoteChokingUs);
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
        check(!f.remote().remoteChokingUs && f.remote().remoteInterestedInUs);
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
        check(network_.peer(2).swarmState(1).connections.at(1).remoteChokingUs == expected_);
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
    check(network.peer(2).swarmState(1).connections.at(1).remoteChokingUs);
    simulation.run();
    check(observations == 3);
    check(!network.peer(2).swarmState(1).connections.at(1).remoteChokingUs);
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
        a.joinSwarm(swarm);
        b.joinSwarm(swarm);
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
        check(f.remote().remoteChokingUs);
    });
    f.observe(0.051, [&] {
        const auto state = f.network.transmissionState(1, 2);
        check(state.active && state.pendingCount == 0); // Second started at 0.05.
        check(f.remote().remoteChokingUs); // First has not arrived.
    });
    f.observe(0.141, [&] {
        check(!f.network.transmissionState(1, 2).active);
        check(f.remote().remoteChokingUs);
        check(f.remote().remoteBitfield[0] == 0);
    });
    f.observe(1.049, [&] { check(f.remote().remoteChokingUs); });
    f.observe(1.051, [&] {
        check(!f.remote().remoteChokingUs);
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

void queuedHandshakeResponse()
{
    HandshakeFixture f;
    // Existing PIECE size/no-op behavior provides a two-second reverse transmission.
    f.network.send(1, 2, 1, Message(MessageType::Piece, PiecePayload{0, 0, 112}));
    f.network.send(1, 1, 2, handshake(f.swarm));
    f.simulation.schedule(std::make_unique<CheckEvent>(1.189, [&] {
        const auto& b = f.state().connections.at(1);
        check(b.handshakeReceived && !b.handshakeSent && !b.handshakeComplete());
        check(f.network.transmissionState(2, 1).pendingCount == 1);
        f.deliver(handshake(f.swarm));
        f.deliver(handshake(f.swarm));
        check(f.network.transmissionState(2, 1).pendingCount == 1);
        const auto before = f.state();
        rejects([&] { f.deliver(Message(MessageType::Unchoke)); });
        checkUnchanged(f.state(), before);
    }));
    f.simulation.schedule(std::make_unique<CheckEvent>(2.001, [&] {
        check(f.state().connections.at(1).handshakeSent);
        check(f.state().connections.at(1).bitfieldSent);
        check(f.network.transmissionState(2, 1).pendingCount == 1); // Automatic BITFIELD.
        check(f.network.transmissionState(2, 1).active);
    }));
    f.simulation.run();
    check(f.state().connections.at(1).handshakeComplete());
    check(f.network.peer(1).swarmState(1).connections.at(2).handshakeComplete());
    check(!f.network.transmissionState(2, 1).active);
    check(std::abs(f.simulation.currentTime() - 3.4) < 1e-12);
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
    // Exactly one 7-byte BITFIELD per direction: B arrives at 2.488, A at 2.588.
    check(std::abs(f.simulation.currentTime() - 2.588) < 1e-12);

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
    f.network.deliver(2, 1, 2, Message(MessageType::Request, RequestPayload{}));
    f.network.deliver(1, 3, 2, Message(MessageType::Request, RequestPayload{}));
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
    check(std::abs(f.simulation.currentTime() - 1.4) < 1e-12);
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
    check(remote.remoteChokingUs && !remote.remoteInterestedInUs);
    check(remote.remoteBitfield == std::vector<std::uint8_t>({0, 0}));
    f.deliver(Message(MessageType::Unchoke));
    check(!f.state().connections.at(1).remoteChokingUs);
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
    // The existing REQUEST no-op creates a default connection, allowing us
    // to check an existing incomplete connection without adding protocol behavior.
    f.deliver(Message(MessageType::Request, RequestPayload{}));
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
        if (existingConnection) f.deliver(Message(MessageType::Request, RequestPayload{}));
        const auto before = f.state();
        for (const auto& message : messages) {
            rejects([&] { f.deliver(message); });
            checkUnchanged(f.state(), before);
        }
    }
    f.complete();
    for (const auto& message : messages) f.deliver(message);
    check(f.state().connections.at(1).handshakeComplete());
    check(!f.state().connections.at(1).remoteChokingUs);
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
    check(!f.state().connections.at(1).remoteChokingUs);
    check(std::abs(f.simulation.currentTime() - 2.588) < 1e-12);
}
} // namespace

int main()
{
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
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
