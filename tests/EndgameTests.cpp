#include "EndgameAssertions.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <source_location>
#include <stdexcept>
#include <tuple>
#include "simulator/Network.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/TransmissionStartEvent.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
using namespace simulator;
namespace {
void check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) throw std::runtime_error("Endgame check at line " + std::to_string(at.line()));
}
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}
class Action : public Event {
public:
    Action(double time, std::function<void()> f) : Event(time), f_(std::move(f)) {}
    void execute() override { f_(); }
private: std::function<void()> f_;
};
struct Fixture {
    Swarm swarm;
    Simulation sim;
    Network net;
    std::vector<Peer> peers() {
        std::vector<Peer> result;
        for (PeerId id : {1u, 2u, 3u}) {
            result.emplace_back(id, 1e7, 1e7);
            result.back().joinSwarm(swarm, {static_cast<std::uint8_t>(id == 2 ? 0 : 0x80)});
        }
        for (PeerId id : {1u, 3u}) {
            auto& a = result[1]; auto& b = result[id - 1];
            for (const auto pair : {std::pair{&a, &b}, std::pair{&b, &a}}) {
                pair.first->markHandshakeSent(swarm, pair.second->id());
                pair.first->receiveMessage(swarm, pair.second->id(),
                    Message(MessageType::Handshake, HandshakePayload{swarm.infoHash(), {}}), &pair.second->protocolId());
                auto& c = const_cast<PeerSwarmState&>(pair.first->swarmState(1)).connections.at(pair.second->id());
                c.bitfieldSent = true;
                c.weAreInterestedInRemote = c.remoteInterestedInUs = true;
                c.weAreChokingRemote = false;
                c.remoteIsChokingUs = false;
                if (pair.first->id() == 2) c.remoteBitfield = {0x80};
            }
        }
        return result;
    }
    explicit Fixture(unsigned size = 16384, double slowRate = 1e5, double slowLatency = .01)
        : swarm(1, InfoHash{1}, size, size),
          net(sim, peers(), {Link(1, 2, 1e6, .01), Link(2, 3, slowRate, slowLatency)}, {swarm}) {}
    PeerSwarmState& state(PeerId id = 2) {
        return const_cast<PeerSwarmState&>(net.peer(id).swarmState(1));
    }
    PeerConnectionState& download(PeerId remote) { return state().connections.at(remote); }
    PeerConnectionState& upload(PeerId remote) { return state(remote).connections.at(2); }
    void at(double t, std::function<void()> f) { sim.schedule(std::make_unique<Action>(t, std::move(f))); }
    void start() { net.deliver(1, 1, 2, Message(MessageType::Unchoke)); }
    void drained() {
        check(net.activeTransmissions().empty());
        for (PeerId id : {1u, 2u, 3u}) for (const auto& [remote, c] : state(id).connections) {
            check(c.scheduledRequests.empty() && c.outgoingRequests.empty());
            check(c.retiredRequests.empty() && c.acceptedRequests.empty() && c.committedRequests.empty());
            check(!net.transmissionState(id, remote).active && net.transmissionState(id, remote).pendingCount == 0);
        }
    }
};
void normalAndCapacity() {
    Fixture f(12 * 16384);
    f.start();
    f.net.deliver(1, 3, 2, Message(MessageType::Unchoke));
    check(f.download(1).scheduledRequests.size() == 5);
    check(f.download(3).scheduledRequests.size() == 5);
    for (const auto& a : f.download(1).scheduledRequests)
        for (const auto& b : f.download(3).scheduledRequests) check(a != b);
    std::set<std::tuple<PeerId, unsigned, unsigned, unsigned>> sent;
    unsigned duplicates = 0;
    std::set<std::tuple<unsigned, unsigned, unsigned>> blocks;
    f.sim.setEventObserver([&](const Event& event) {
        const auto* starting = dynamic_cast<const TransmissionStartEvent*>(&event);
        const auto sending = starting && starting->details().sender == 2
            && starting->details().messageType == MessageType::Request
            ? std::optional<RequestPayload>(std::get<RequestPayload>(starting->message().payload())) : std::nullopt;
        endgame_test::reservations(f.swarm, f.state(), sending);
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
            start && start->details().messageType == MessageType::Request) {
            const auto b = std::get<RequestPayload>(start->message().payload());
            check(sent.emplace(start->details().receiver, b.index, b.begin, b.length).second);
            if (!blocks.emplace(b.index, b.begin, b.length).second) {
                check(endgame_test::fullyCovered(f.swarm, f.state(), b));
                ++duplicates;
            }
        }
    });
    f.sim.run();
    check(duplicates > 0 && f.state().localBitfield[0] == 0x80);
    f.drained();
}
void firstWins(bool requestStillInFlight, bool queued) {
    Fixture f(16384, queued ? 1e6 : 1e5, requestStillInFlight ? 1 : .01);
    const RequestPayload block{0, 0, 16384};
    if (queued) {
        // Valid control traffic occupies the losing upload direction until after CANCEL.
        for (int i = 0; i < 2000; ++i)
            f.net.send(1, 3, 2, Message(MessageType::Cancel, CancelPayload{0, 0, 1}));
    }
    unsigned cancels = 0, pieces = 0, haves = 0;
    bool sawRetired = false, checkedLate = false, checkedRequestBeforeCancel = false;
    std::set<PeerId> providers;
    f.sim.setEventObserver([&](const Event& event) {
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event)) {
            const auto d = start->details();
            if (d.messageType == MessageType::Request) check(providers.insert(d.receiver).second);
            if (d.messageType == MessageType::Cancel && d.sender == 2) {
                const auto c = std::get<CancelPayload>(start->message().payload());
                check(c.index == block.index && c.begin == block.begin && c.length == block.length);
                check(d.receiver == 3);
                check(f.download(3).outgoingRequests.empty() && f.download(3).scheduledRequests.empty());
                check(f.download(3).retiredRequests == std::vector<RequestPayload>{block});
                sawRetired = true;
                ++cancels;
            }
            if (d.messageType == MessageType::Have && d.sender == 2) ++haves;
            if (d.messageType == MessageType::Piece) ++pieces;
        }
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event);
            arrival && arrival->details().messageType == MessageType::Cancel
            && arrival->details().sender == 2 && arrival->details().receiver == 3) {
            // Even when the downloader won before REQUEST arrival, the provider
            // accepted and committed its response before learning of cancellation.
            check(f.upload(3).acceptedRequests == std::vector<RequestPayload>{block});
            check(f.upload(3).committedRequests == std::vector<RequestPayload>{block});
            checkedRequestBeforeCancel = true;
        }
        if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event);
            arrival && arrival->details().messageType == MessageType::Piece && arrival->details().sender == 3) {
            check(sawRetired);
            const auto coverage = f.state().receivedBlocks;
            const auto bits = f.state().localBitfield;
            const auto useful = f.download(3).downloadedInWindow;
            f.at(event.time(), [&, coverage, bits, useful] {
                check(f.state().receivedBlocks == coverage && f.state().localBitfield == bits);
                check(f.download(3).downloadedInWindow == useful && f.upload(3).uploadedInWindow == 0);
                check(f.download(3).retiredRequests.empty());
                checkedLate = true;
            });
        }
    });
    f.start();
    check(f.download(1).scheduledRequests == std::vector<RequestPayload>{block});
    check(f.download(3).scheduledRequests == std::vector<RequestPayload>{block});
    f.sim.run();
    check(cancels == 1 && haves == 2 && providers.size() == 2);
    check(pieces == 2);
    check(checkedLate && checkedRequestBeforeCancel);
    check(f.state().receivedBlocks.at(0) == std::vector<BlockRange>{{0, 16384}});
    f.drained();
    rejects([&] { f.net.deliver(1, 3, 2, Message(MessageType::Piece, PiecePayload{0, 0, 16384})); });
}
void earlyCancel() {
    Fixture f;
    const RequestPayload block{0, 0, 16384};
    f.download(1).outgoingRequests.push_back(block);
    f.net.send(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 0, 16384}));
    check(f.download(1).outgoingRequests.empty());
    check(f.download(1).retiredRequests == std::vector<RequestPayload>{block});
    unsigned sends = 0, starts = 0;
    f.sim.setEventObserver([&](const Event& e) {
        if (const auto* send = dynamic_cast<const SendMessageEvent*>(&e);
            send && send->details().messageType == MessageType::Piece) ++sends;
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&e);
            start && start->details().messageType == MessageType::Piece) ++starts;
    });
    // Both arrivals are already queued: CANCEL executes before the newly scheduled PIECE send.
    f.sim.schedule(std::make_unique<MessageArrivalEvent>(0, f.net, 1, 2, 1, Message(MessageType::Request, block)));
    f.sim.schedule(std::make_unique<MessageArrivalEvent>(0, f.net, 1, 2, 1,
        Message(MessageType::Cancel, CancelPayload{0, 0, 16384})));
    f.sim.run();
    check(sends == 1 && starts == 0 && f.upload(1).acceptedRequests.empty());
    check(f.upload(1).committedRequests.empty() && f.state().receivedBlocks.empty());
    f.net.deliver(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 0, 16384}));
    f.drained();
}
void manualCancel(bool queued) {
    Fixture f;
    const RequestPayload block{0, 0, 16384};
    if (queued) for (int i = 0; i < 2000; ++i)
        f.net.send(1, 1, 2, Message(MessageType::Cancel, CancelPayload{0, 0, 1}));
    f.net.send(1, 2, 1, Message(MessageType::Request, block));
    unsigned pieces = 0;
    f.sim.setEventObserver([&](const Event& event) {
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event);
            start && start->details().messageType == MessageType::Piece) ++pieces;
    });
    f.at(.03, [&] {
        check(f.upload(1).committedRequests == std::vector<RequestPayload>{block});
        // A different remote or block must not retire this outstanding request.
        f.net.send(1, 2, 3, Message(MessageType::Cancel, CancelPayload{0, 0, 16384}));
        f.net.send(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 1, 1}));
        check(f.download(1).outgoingRequests == std::vector<RequestPayload>{block});
        check(f.download(1).retiredRequests.empty() && f.download(3).retiredRequests.empty());
        rejects([&] { f.net.send(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 0, 0})); });
        check(f.download(1).outgoingRequests == std::vector<RequestPayload>{block});
        f.net.send(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 0, 16384}));
        f.net.send(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 0, 16384}));
        check(f.download(1).outgoingRequests.empty() && f.download(1).scheduledRequests.empty());
        check(f.download(1).retiredRequests == std::vector<RequestPayload>{block});
        rejects([&] { f.net.deliver(1, 3, 2, Message(MessageType::Piece, PiecePayload{0, 0, 16384})); });
        rejects([&] { f.net.deliver(1, 1, 2, Message(MessageType::Piece, PiecePayload{0, 0, 1})); });
        // Notifications cannot request the same block again while retirement is pending.
        f.net.deliver(1, 1, 2, Message(MessageType::Unchoke));
        check(f.download(1).scheduledRequests.empty());
    });
    f.sim.run();
    check(pieces == 1 && f.state().localBitfield[0] == 0 && f.state().receivedBlocks.empty());
    check(f.download(1).downloadedInWindow == 0 && f.upload(1).uploadedInWindow == 0);
    f.drained();
    rejects([&] { f.net.deliver(1, 1, 2, Message(MessageType::Piece, PiecePayload{0, 0, 16384})); });
}
void manualCancelReleasesCapacity() {
    Fixture f(7 * 16384);
    const RequestPayload canceled{0, 0, 16384};
    f.net.send(1, 2, 1, Message(MessageType::Request, canceled));
    f.net.send(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 0, 16384}));
    f.start();
    check(f.download(1).retiredRequests == std::vector<RequestPayload>{canceled});
    check(f.download(1).outgoingRequests.empty() && f.download(1).scheduledRequests.size() == 5);
    for (const auto& block : f.download(1).scheduledRequests) check(block.begin >= 16384);
    // Another provider can reserve the canceled gap immediately; retirement is
    // neither a cross-peer reservation nor a pipeline slot on the old provider.
    f.net.deliver(1, 3, 2, Message(MessageType::Unchoke));
    check(f.download(3).scheduledRequests.front() == canceled);
    f.sim.run();
    check(f.state().localBitfield[0] == 0x80);
    f.drained();
}
void cancelValidation() {
    Fixture f;
    for (const auto c : {CancelPayload{0, 0, 0}, CancelPayload{1, 0, 1},
                        CancelPayload{0, 16384, 1}, CancelPayload{0, 1, 0xffffffffu}}) {
        rejects([&] { f.net.send(1, 2, 1, Message(MessageType::Cancel, c)); });
        rejects([&] { f.net.deliver(1, 2, 1, Message(MessageType::Cancel, c)); });
    }
    f.download(1).handshakeReceived = false;
    rejects([&] { f.net.send(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 0, 1})); });
    f.upload(1).handshakeReceived = false;
    rejects([&] { f.net.deliver(1, 2, 1, Message(MessageType::Cancel, CancelPayload{0, 0, 1})); });
}
void scheduledDuplicateCanceled() {
    Fixture f;
    const RequestPayload block{0, 0, 16384};
    f.download(1).outgoingRequests.push_back(block);
    f.upload(1).acceptedRequests.push_back(block);
    f.net.deliver(1, 3, 2, Message(MessageType::Unchoke));
    check(f.download(3).scheduledRequests == std::vector<RequestPayload>{block});
    f.net.deliver(1, 1, 2, Message(MessageType::Piece, PiecePayload{0, 0, 16384}));
    check(f.download(3).scheduledRequests.empty() && f.download(3).retiredRequests.empty());
    unsigned requests = 0;
    f.sim.setEventObserver([&](const Event& e) {
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&e);
            start && start->details().messageType == MessageType::Request) ++requests;
    });
    f.sim.run();
    check(requests == 0);
    f.drained();
}
void leaveWithRetired() {
    Fixture f;
    bool left = false;
    f.sim.setEventObserver([&](const Event& e) {
        if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&e);
            start && start->details().messageType == MessageType::Cancel && start->details().sender == 2) {
            check(!f.download(3).retiredRequests.empty());
            f.at(e.time(), [&] { f.net.leaveSwarm(1, 3); left = true; });
        }
    });
    f.start(); f.sim.run();
    check(left && !f.state().connections.contains(3));
    check(f.state(3).connections.empty() && f.net.activeTransmissions().empty());
}
}
int main() {
    try {
        normalAndCapacity();
        firstWins(false, false);
        firstWins(false, true);
        firstWins(true, false);
        earlyCancel();
        manualCancel(false);
        manualCancel(true);
        manualCancelReleasesCapacity();
        cancelValidation();
        scheduledDuplicateCanceled();
        leaveWithRetired();
        std::cout << "Endgame/CANCEL tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
