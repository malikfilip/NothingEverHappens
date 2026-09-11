#include <cmath>
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

int existingDeliveryRegression()
{
    try {
        const Swarm swarm(1, {}, 9);
        const Swarm other(2, {}, 8);
        Peer receiver(2, 500, 500);
        rejects([&] { receiver.receiveMessage(swarm, 1, Message(MessageType::Unchoke)); });
        receiver.joinSwarm(swarm);
        receiver.joinSwarm(other);
        auto receive = [&](Message message) { receiver.receiveMessage(swarm, 1, message); };
        auto remote = [&]() -> const PeerConnectionState& {
            return receiver.swarmState(1).connections.at(1);
        };

        rejects([&] { receive(Message(MessageType::Have, HavePayload{9})); });
        rejects([&] { receive(Message(MessageType::Bitfield, BitfieldPayload{{}})); });
        check(receiver.swarmState(1).connections.empty());
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
        receiver.receiveMessage(other, 1, Message(MessageType::Bitfield, BitfieldPayload{{0xff}}));
        check(receiver.swarmState(2).connections.at(1).remoteBitfield[0] == 0xff);
        for (const auto& message : {
                Message(MessageType::Handshake, HandshakePayload{}),
                Message(MessageType::Request, RequestPayload{}),
                Message(MessageType::Piece, PiecePayload{}),
                Message(MessageType::Cancel, CancelPayload{})}) {
            receive(message);
        }
        check(remote().remoteChokingUs && !remote().remoteInterestedInUs);
        check(remote().remoteBitfield == std::vector<std::uint8_t>({0x40, 0}));

        Peer sender(1, 1000, 1000);
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

    Fixture() { receiver.joinSwarm(swarm); }
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
    Peer sender(1, 1000, 1000);
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
} // namespace

int main()
{
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
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
