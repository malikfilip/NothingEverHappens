#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <set>
#include "simulator/Network.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/RechokeEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/TransmissionStartEvent.hpp"
using namespace simulator;
namespace {
void check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Choking assertion at line " + std::to_string(where.line()));
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
class Action : public Event {
public:
    Action(double t, std::function<void()> f) : Event(t), f_(std::move(f)) {}
    void execute() override { f_(); }
private: std::function<void()> f_;
};
struct Fixture {
    Swarm a{1, InfoHash{1}, 2097152, 1048576}, b{2, InfoHash{2}, 2097152, 1048576};
    Simulation simulation;
    Network network;
    bool seed;
    std::vector<Peer> peers(bool seed) {
        std::vector<Peer> result;
        for (PeerId id = 1; id <= 7; ++id) {
            result.emplace_back(id, 1e7, 1e7);
            for (const auto* swarm : {&a, &b})
                result.back().joinSwarm(*swarm, {static_cast<std::uint8_t>(
                    id == 1 ? (seed ? 0xc0 : 0x40) : (seed ? 0 : 0x80))});
        }
        for (const auto* swarm : {&a, &b}) for (PeerId id = 2; id <= 7; ++id) {
            auto& local = result[0]; auto& remote = result[id - 1];
            local.markHandshakeSent(*swarm, id); remote.markHandshakeSent(*swarm, 1);
            local.receiveMessage(*swarm, id, Message(MessageType::Handshake, HandshakePayload{swarm->infoHash(), {}}), &remote.protocolId());
            remote.receiveMessage(*swarm, 1, Message(MessageType::Handshake, HandshakePayload{swarm->infoHash(), {}}), &local.protocolId());
        }
        return result;
    }
    explicit Fixture(bool isSeed = false, BitTorrentSettings settings = {}, std::uint64_t simulationSeed = 0)
        : simulation(false, simulationSeed), network(simulation, peers(isSeed),
            {Link(1,2,1e7,.01), Link(1,3,1e7,.01), Link(1,4,1e7,.01),
             Link(1,5,1e7,.01), Link(1,6,1e7,.01), Link(1,7,1e7,.01)}, {a,b}, 50, 1800, settings),
          seed(isSeed) {
        at(81, [&] { stop(); });
    }
    PeerSwarmState& state(PeerId id = 1, SwarmId swarm = 1) {
        return const_cast<PeerSwarmState&>(network.peer(id).swarmState(swarm));
    }
    // Timing/budget tests may start with a specified assignment independently of
    // candidate-selection tests. Startup still emits the actual wire transition.
    void presetOptimistic(PeerId remote) {
        state().choking.optimistic = remote;
        state().connections.at(remote).optimisticConsidered = true;
    }
    void at(double t, std::function<void()> f) { simulation.schedule(std::make_unique<Action>(t, std::move(f))); }
    void interest(PeerId id, bool interested = true, SwarmId swarm = 1) {
        network.deliver(swarm, id, 1, Message(interested ? MessageType::Interested : MessageType::NotInterested));
    }
    void all(SwarmId swarm = 1) { for (PeerId id = 2; id <= 7; ++id) interest(id, true, swarm); }
    void stop() {
        for (SwarmId swarm : {1u,2u}) for (PeerId id=2; id<=7; ++id) {
            if (!network.peer(id).isActiveInSwarm(swarm)) continue;
            interest(id, false, swarm);
            network.deliver(swarm, 1, id, Message(MessageType::NotInterested));
        }
    }
    void invariants(SwarmId swarm = 1) {
        const auto& s = state(1, swarm);
        unsigned interestedUnchoked = 0;
        for (const auto& [id, c] : s.connections)
            interestedUnchoked += c.remoteInterestedInUs && !c.weAreChokingRemote;
        check(interestedUnchoked <= 4);
        check(!s.choking.optimistic || !s.choking.preferred.contains(*s.choking.optimistic));
        for (const auto& [id,c] : s.connections)
            check(!c.weAreChokingRemote == (s.choking.preferred.contains(id) || s.choking.optimistic == id));
    }
    void payload(PeerId from, PeerId to, std::uint32_t begin, std::uint32_t bytes, SwarmId swarm = 1) {
        const std::uint32_t piece = from == 1 && !seed ? 1 : 0;
        const RequestPayload block{piece, begin, bytes};
        state(from,swarm).connections.at(to).weAreChokingRemote = false;
        state(from,swarm).connections.at(to).acceptedRequests.push_back(block);
        state(to,swarm).connections.at(from).outgoingRequests.push_back(block);
        network.send(swarm, from, to, Message(MessageType::Piece, PiecePayload{piece,begin,bytes}));
    }
};
void bootstrap() {
    Fixture f;
    unsigned unchokes=0, regular=0;
    f.simulation.setEventObserver([&](const Event& e) {
        if (const auto* s=dynamic_cast<const SendMessageEvent*>(&e);
            s && s->details().sender==1 && s->details().messageType==MessageType::Unchoke) ++unchokes;
        if (const auto* r=dynamic_cast<const RechokeEvent*>(&e); r && r->regularDue()) ++regular;
    });
    for (PeerId id=2; id<=7; ++id) f.at(2.347 + (id-2)*.003, [&,id] {
        f.interest(id);
        f.interest(id); // Duplicate must not reset the cycle or repeat wire transitions.
        f.invariants();
        check(near(f.state().choking.nextRegularDeadline,12.347));
        check(near(f.state().choking.nextOptimisticDeadline,32.347));
        check(f.state().choking.preferred == std::set<PeerId>{2});
        if (id!=2) check(f.state().connections.at(id).weAreChokingRemote == (f.state().choking.optimistic != id));
    });
    f.at(2.4,[&] { check(unchokes==2 && regular==0); check(f.state().choking.optimistic.has_value()); });
    f.at(12.348,[&] { check(regular==1); check(f.state().choking.preferred==std::set<PeerId>({2,3,4})); });
    f.simulation.run();
}
void vacancies() {
    Fixture f; f.all();
    PeerId newcomer=0;
    f.at(11,[&] {
        check(f.state().choking.preferred == std::set<PeerId>({2,3,4}));
        const auto preferred = f.state().choking.preferred;
        const auto optimistic = f.state().choking.optimistic;
        f.state().connections.at(2).downloadedInWindow=123;
        f.interest(2,false);
        check(!f.state().connections.at(2).weAreChokingRemote);
        check(f.state().choking.preferred==preferred && f.state().choking.optimistic==optimistic);
        check(f.state().connections.at(2).downloadedInWindow==123);
        check(f.state().choking.nextRegularDeadline==20 && f.state().choking.nextOptimisticDeadline==30);
        for (PeerId id=5; id<=7; ++id)
            if (f.state().connections.at(id).weAreChokingRemote) { newcomer=id; break; }
        check(newcomer!=0);
        f.interest(newcomer,false);
        f.interest(newcomer); // Choked newcomer cannot take the vacancy, even with the best rate.
        f.state().connections.at(newcomer).downloadedInWindow=1000;
        check(f.state().connections.at(newcomer).weAreChokingRemote);
        check(f.state().choking.preferred==preferred);
        f.invariants();
    });
    f.at(20.001,[&] {
        check(f.state().choking.preferred.contains(newcomer));
        check(!f.state().connections.at(newcomer).weAreChokingRemote);
        f.invariants();
    });
    f.simulation.run();
}
void interestedSlotAccounting() {
    Fixture f; f.presetOptimistic(3);
    for (const PeerId id : {2u, 4u, 5u, 6u}) f.interest(id);
    f.at(10.001,[&] {
        check(f.state().choking.optimistic == 3);
        check(f.state().choking.preferred == std::set<PeerId>({2,4,5,6}));
        f.invariants(); // Four regular interested PLUS an uninterested optimistic peer.
        const auto deadline = f.state().choking.pendingWakeup;
        f.interest(3);
        check(f.state().choking.optimistic == 3);
        check(f.state().choking.preferred == std::set<PeerId>({2,4,5}));
        check(f.state().connections.at(6).weAreChokingRemote);
        f.invariants();
        f.interest(3, false);
        check(f.state().choking.optimistic == 3 && !f.state().connections.at(3).weAreChokingRemote);
        check(f.state().choking.preferred == std::set<PeerId>({2,4,5}));
        check(f.state().choking.pendingWakeup == deadline);
        f.invariants();
    });
    f.simulation.run();
}
void preUnchokedInterest(bool seed) {
    Fixture f(seed); f.presetOptimistic(3);
    for (const PeerId id : {2u, 4u, 5u, 6u}) f.interest(id);
    // Real delivered payload establishes rates; uninterested peer 7 is fastest.
    unsigned begin = 0;
    for (const auto [id, bytes] : {std::pair{2u,400u}, {4u,300u}, {5u,200u}, {6u,100u}, {7u,500u}}) {
        f.payload(seed ? 1 : id, seed ? id : 1, begin, bytes);
        begin += 1000;
    }
    f.at(10.001, [&] {
        check(f.state().choking.optimistic == 3);
        check(f.state().choking.preferred == std::set<PeerId>({2,4,5,6,7}));
        check(!f.state().connections.at(7).remoteInterestedInUs);
        check(!f.state().connections.at(7).weAreChokingRemote);
        f.invariants();
    });
    f.at(11, [&] {
        const auto deadline = f.state().choking.pendingWakeup;
        const auto rate = f.state().connections.at(7).recentDownloadRate;
        f.interest(7);
        check(f.state().choking.preferred == std::set<PeerId>({2,4,5,7}));
        check(f.state().connections.at(6).weAreChokingRemote);
        check(f.state().choking.optimistic == 3);
        f.interest(3); // The optimistic assignment is protected; worst regular loses.
        check(f.state().choking.preferred == std::set<PeerId>({2,4,7}));
        check(f.state().connections.at(5).weAreChokingRemote);
        f.interest(7, false); // Good-rate uninterested peer stays pre-unchoked.
        check(f.state().choking.preferred == std::set<PeerId>({2,4,7}));
        check(!f.state().connections.at(7).weAreChokingRemote);
        f.interest(7);
        check(f.state().choking.preferred == std::set<PeerId>({2,4,7}));
        check(f.state().choking.pendingWakeup == deadline);
        check(f.state().connections.at(7).recentDownloadRate == rate);
        check(f.state().choking.windowStart == 10);
        f.invariants();
    });
    f.simulation.run();
}
void uninterestedRotation(bool interestedCandidate) {
    Fixture f(false, {10,25}); f.presetOptimistic(3);
    for (const PeerId id : {2u,4u,5u,6u}) f.interest(id);
    check(f.state().choking.optimistic == 3);
    f.at(24, [&] {
        check(f.state().choking.preferred == std::set<PeerId>({2,4,5,6}));
        check(!f.state().connections.at(3).weAreChokingRemote);
    });
    f.at(24.5, [&] {
        if (interestedCandidate) f.interest(7);
        check(f.state().connections.at(7).weAreChokingRemote);
        check(f.state().choking.preferred == std::set<PeerId>({2,4,5,6}));
    });
    f.at(25.001, [&] {
        check(f.state().choking.optimistic == 7);
        check(f.state().choking.preferred == (interestedCandidate
            ? std::set<PeerId>({2,4,5}) : std::set<PeerId>({2,4,5,6})));
        check(f.state().connections.at(3).weAreChokingRemote);
        check(f.state().choking.windowStart == 20);
        check(f.state().choking.nextRegularDeadline == 30);
        f.invariants();
    });
    f.simulation.run();
}
void deadlines(double regular, double optimistic) {
    Fixture f(false,{regular,optimistic});
    std::vector<double> regularTimes, optimisticTimes, wakes;
    f.simulation.setEventObserver([&](const Event& e) {
        if (const auto* r=dynamic_cast<const RechokeEvent*>(&e); r && e.time()<62.347) {
            wakes.push_back(e.time());
            if(r->regularDue()) regularTimes.push_back(e.time());
            if(r->optimisticDue()) optimisticTimes.push_back(e.time());
        }
    });
    f.at(2.347,[&]{f.all();});

    f.at(62.346,[&] {
        std::size_t count=0;
        for(double t=2.347+regular;t<62.346;t+=regular) {
            check(count<regularTimes.size() && near(regularTimes[count++],t));
        }
        check(count==regularTimes.size());
        count=0;
        for(double t=2.347+optimistic;t<62.346;t+=optimistic) {
            check(count<optimisticTimes.size() && near(optimisticTimes[count++],t));
        }
        check(count==optimisticTimes.size());
        check(std::adjacent_find(wakes.begin(),wakes.end())==wakes.end());
    });
    f.simulation.run();
}
void optimisticOnly() {
    Fixture f(false,{10,25}); f.all();
    std::optional<PeerId> previous;
    f.at(24,[&] {
        f.state().connections.at(2).downloadedInWindow=99;
        previous=f.state().choking.optimistic;
    });
    f.at(25.001,[&] {
        check(f.state().choking.preferred==std::set<PeerId>({2,3,4}));
        check(f.state().choking.optimistic && f.state().choking.optimistic!=previous);
        check(f.state().connections.at(2).downloadedInWindow==99);
        check(f.state().choking.windowStart==20 && f.state().choking.nextRegularDeadline==30);
    });
    f.at(49,[&] { previous=f.state().choking.optimistic; });
    f.at(50.001,[&] { check(f.state().choking.optimistic!=previous); f.invariants(); });
    f.simulation.run();
}
void ranking(bool seed) {
    Fixture f(seed,{5,25}); f.presetOptimistic(3); f.all();
    for(PeerId id=2;id<=7;++id)
        f.payload(seed?1:id,seed?id:1,(id-2)*1000,(id-1)*100);
    f.at(5.001,[&] {
        check(f.state().choking.preferred==std::set<PeerId>({5,6,7}));
        check(f.state().choking.optimistic==3); // Existing optimistic assignment survives ranking.
        for(PeerId id=2;id<=7;++id) {
            const auto& c=f.state().connections.at(id);
            check(near(seed?c.recentUploadRate:c.recentDownloadRate,(id-1)*20));
        }
        f.invariants();
    });
    f.simulation.run();
}
void rollingBurstRanking() {
    Fixture f; f.all();
    for (PeerId id=2;id<=5;++id) f.payload(id,1,(id-2)*10000,(11-id)*1000);
    f.at(10.001,[&]{check(f.state().choking.preferred==std::set<PeerId>({2,3,4}));});
    f.at(19,[&]{f.payload(7,1,50000,1000);});
    f.at(20.001,[&]{
        check(f.state().connections.at(7).recentDownloadRate==50);
        check(f.state().connections.at(2).recentDownloadRate==450);
        check(f.state().choking.preferred==std::set<PeerId>({2,3,4}));
    });
    f.simulation.run();
}
void combinedPromotion() {
    Fixture f(false,{5,5}); f.presetOptimistic(3); f.all();
    unsigned optimisticWireTransitions=0;
    f.simulation.setEventObserver([&](const Event& e){
        if(const auto* s=dynamic_cast<const SendMessageEvent*>(&e);
            s && e.time()==5 && s->details().sender==1 && s->details().receiver==3
            && (s->details().messageType==MessageType::Choke || s->details().messageType==MessageType::Unchoke))
            ++optimisticWireTransitions;
    });
    f.at(4,[&]{f.state().connections.at(5).downloadedInWindow=1000;});
    f.at(5.001,[&]{
        check(f.state().choking.preferred==std::set<PeerId>({2,3,5}));
        check(f.state().choking.optimistic && !f.state().choking.preferred.contains(*f.state().choking.optimistic));
        check(optimisticWireTransitions==0);
        f.invariants();
    });
    f.simulation.run();
}
void rollingRates(bool seed) {
    Fixture f(seed,{5,12}); f.interest(2);
    auto transfer=[&](unsigned begin,unsigned bytes){ f.payload(seed?1:2,seed?2:1,begin,bytes); };
    auto rate=[&]{ const auto& c=f.state().connections.at(2); return seed?c.recentUploadRate:c.recentDownloadRate; };
    f.at(1,[&]{transfer(0,1000);});
    f.at(5.001,[&]{check(near(rate(),200));});
    f.at(6,[&]{transfer(1000,200);});
    f.at(10.001,[&]{check(near(rate(),120));});
    f.at(11,[&]{transfer(1200,600);});
    f.at(12.001,[&] {
        check(near(rate(),120));
        const auto& c=f.state().connections.at(2);
        check((seed?c.uploadedInWindow:c.downloadedInWindow)==600);
    });
    f.at(15.001,[&]{check(near(rate(),80));});
    f.at(25.001,[&]{check(rate()==0);});
    f.simulation.run();
}
void stableClockAndDuplicates() {
    Fixture f(false,{5,12}); f.interest(2);
    const auto cycle=f.state().choking.cycleGeneration;
    f.simulation.schedule(std::make_unique<RechokeEvent>(5,f.network,1,1));
    f.at(1,[&] {
        f.state().connections.at(2).downloadedInWindow=900;
        f.interest(2,false);
        check(f.state().choking.eventPending);
        check(f.state().choking.nextRegularDeadline==5);
    });
    f.at(2.347,[&] {
        f.interest(2); f.interest(2);
        check(f.state().choking.cycleGeneration==cycle);
        check(f.state().connections.at(2).downloadedInWindow==900);
        check(f.state().choking.nextRegularDeadline==5);
    });
    f.at(5.001,[&] {
        check(f.state().connections.at(2).recentDownloadRate==180);
        check(f.state().choking.pendingWakeup==10);
        f.interest(2,false);
    });
    f.at(10.001,[&] {
        check(!f.state().choking.eventPending); // Idle wake-ups omitted, phase retained.
        check(f.state().connections.at(2).recentDownloadRate==90);
    });
    f.at(11,[&] {
        const auto preferred=f.state().choking.preferred;
        f.interest(2);
        check(f.state().choking.preferred==preferred);
        check(f.state().choking.pendingWakeup==12);
        check(f.state().choking.nextRegularDeadline==15);
        check(f.state().connections.at(2).recentDownloadRate==90);
        check(f.state().choking.cycleGeneration==cycle);
    });
    f.at(15.001,[&] { check(f.state().connections.at(2).recentDownloadRate==0); });
    f.simulation.run();
}
void observationsDoNotSelect() {
    Fixture f(false,{5,12}); f.all();
    unsigned transitions=0;
    std::optional<PeerId> expectedOptimistic;
    f.simulation.setEventObserver([&](const Event& event) {
        if (const auto* send=dynamic_cast<const SendMessageEvent*>(&event);
            send && event.time()>5.1 && event.time()<10 && send->details().sender==1
            && (send->details().messageType==MessageType::Choke || send->details().messageType==MessageType::Unchoke))
            ++transitions;
    });
    f.at(6,[&] {
        const auto preferred=f.state().choking.preferred;
        const auto optimistic=f.state().choking.optimistic;
        expectedOptimistic=optimistic;
        PeerId choked=0;
        for (PeerId id=5; id<=7; ++id)
            if (f.state().connections.at(id).weAreChokingRemote) { choked=id; break; }
        check(choked!=0);
        const auto wake=f.state().choking.pendingWakeup;
        // Make a previously choked peer interesting and give it a high measured
        // rate: observations must not even recalculate uninterested pre-unchokes.
        f.interest(choked,false); f.interest(choked,false);
        f.state().connections.at(choked).recentDownloadRate=1000;
        for (int repeat=0; repeat<2; ++repeat) {
            f.network.deliver(1,choked,1,Message(MessageType::Bitfield,BitfieldPayload{{0x40}}));
            f.network.deliver(1,choked,1,Message(MessageType::Have,HavePayload{1}));
        }
        check(f.state().connections.at(choked).remoteBitfield[0]==0x40);
        check(f.state().connections.at(choked).weAreChokingRemote);
        f.interest(choked); f.interest(choked);
        f.interest(*optimistic,false); f.interest(*optimistic,false);
        f.interest(*optimistic); f.interest(*optimistic);
        check(f.state().choking.preferred==preferred && f.state().choking.optimistic==optimistic);
        check(f.state().choking.pendingWakeup==wake);
        f.payload(2,1,0,1000);
        f.invariants();
    });
    f.at(6.1,[&] {
        check(f.state().choking.preferred==std::set<PeerId>({2,3,4}));
        check(f.state().choking.optimistic==expectedOptimistic);
        check(f.state().connections.at(2).downloadedInWindow==1000);
        check(f.state(2).connections.at(1).uploadedInWindow==1000);
        check(f.state().receivedBlocks.at(0).front().end==1000);
        check(f.state().choking.windowStart==5 && f.state().choking.pendingWakeup==10);
        check(transitions==0);
        f.invariants();
    });
    f.at(9.999,[&] { check(transitions==0); });
    f.at(11.999,[&] { expectedOptimistic=f.state().choking.optimistic; });
    f.at(12.001,[&] { check(f.state().choking.optimistic!=expectedOptimistic); });
    f.simulation.run();
}
void idlePhaseAndAccounting() {
    Fixture f(false,{5,12}); f.interest(2);
    f.at(1,[&] { f.payload(2,1,0,1000); f.interest(2,false); });
    f.at(5.001,[&] {
        check(!f.state().choking.eventPending);
        check(f.state().connections.at(2).recentDownloadRate==200);
    });
    f.at(8,[&] { f.payload(2,1,1000,200); });
    f.at(11,[&] { f.payload(2,1,1200,300); });
    f.at(11.1,[&] {
        check(f.state().connections.at(2).recentDownloadRate==120);
        check(f.state().connections.at(2).downloadedPreviousInterval==200);
        check(f.state().connections.at(2).downloadedInWindow==300);
        check(f.state().choking.windowStart==10);
    });
    f.at(39,[&] {
        const auto preferred=f.state().choking.preferred;
        const auto optimistic=f.state().choking.optimistic;
        f.interest(2);
        check(f.state().choking.preferred==preferred && f.state().choking.optimistic==optimistic);
        check(f.state().choking.cycleStart==0);
        check(f.state().choking.nextRegularDeadline==40);
        check(f.state().choking.nextOptimisticDeadline==48);
        check(f.state().choking.windowStart==35);
        check(f.state().connections.at(2).recentDownloadRate==0);
        check(f.state().connections.at(2).downloadedInWindow==0);
    });
    f.at(40.001,[&] { check(f.state().choking.windowStart==40); });
    f.simulation.run();
}
void departureRepairOnly() {
    Fixture f; f.all();
    f.at(11,[&] {
        check(f.state().choking.preferred==std::set<PeerId>({2,3,4}));
        const auto optimistic=f.state().choking.optimistic;
        check(optimistic.has_value());
        f.network.leaveSwarm(1,2);
        check(f.state().choking.preferred==std::set<PeerId>({3,4}));
        check(f.state().choking.optimistic==optimistic);
        f.network.leaveSwarm(1,*optimistic);
        check(f.state().choking.preferred==std::set<PeerId>({3,4}));
        check(f.state().choking.optimistic && f.state().choking.optimistic!=optimistic);
        for (PeerId id=5; id<=7; ++id) if (id!=*optimistic)
            check(f.state().connections.at(id).weAreChokingRemote == (f.state().choking.optimistic!=id));
        check(f.state().choking.pendingWakeup==20 && f.state().choking.nextOptimisticDeadline==30);
        f.invariants();
    });
    f.simulation.run();
}
void usefulOnly() {
    Fixture f;
    f.payload(2,1,0,100);
    f.at(1,[&]{
        check(f.state().connections.at(2).downloadedInWindow==100);
        f.payload(3,1,50,100);
        f.network.send(1,2,1,Message(MessageType::Cancel,CancelPayload{0,0,1}));
    });
    f.at(2,[&]{
        check(f.state().connections.at(3).downloadedInWindow==50);
        check(f.state(3).connections.at(1).uploadedInWindow==50);
        check(f.state().connections.at(2).downloadedInWindow==100);
    });
    f.simulation.run();
}
void isolation() {
    Fixture f; f.all();
    f.at(3,[&]{
        f.all(2);
        f.network.deliver(1,1,2,Message(MessageType::Interested));
    });
    f.at(10.001,[&]{
        check(f.state(1,1).choking.windowStart==10);
        check(f.state(1,2).choking.windowStart==3);
        check(f.state(1,2).choking.nextRegularDeadline==13);
        check(f.state(2,1).choking.nextRegularDeadline==13);
        check(f.state(2,1).choking.windowStart==3);
    });
    f.simulation.run();
}
void validation() {
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double inf=std::numeric_limits<double>::infinity();
    for(const BitTorrentSettings s : {BitTorrentSettings{0,30},{-1,30},{10,0},{10,5},{nan,30},{10,nan},{inf,inf}}) {
        bool rejected=false;
        try { Fixture f(false,s); } catch(const std::invalid_argument&) {rejected=true;}
        check(rejected);
    }
    Fixture f(false,{5,25});
    check(f.network.bitTorrentSettings().regularRechokeInterval==5);
    f.at(1e20,[&]{
        bool rejected=false;
        try{f.interest(2);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected && !f.state().choking.cycleActive && !f.state().choking.eventPending);
    });
    f.simulation.run();
}
std::vector<PeerId> optimisticTrace(std::uint64_t seed, bool reordered = false, bool noisy = false) {
    Fixture f(false,{5,12},seed);
    if (reordered) {
        auto& connections=f.state().connections;
        const auto copy=connections;
        connections.clear(); connections.rehash(97);
        for (PeerId id=7; id>=2; --id) connections.emplace(id,copy.at(id));
    }
    f.all();
    std::vector<PeerId> trace{*f.state().choking.optimistic};
    for (double time : {5.001,12.001,24.001,36.001,48.001,60.001}) f.at(time,[&] {
        const auto optimistic=f.state().choking.optimistic;
        check(optimistic && !f.state().choking.preferred.contains(*optimistic));
        trace.push_back(*optimistic);
        f.invariants();
    });
    if (noisy) for (double time : {6.,13.,26.,38.,49.}) f.at(time,[&] {
        const auto optimistic=f.state().choking.optimistic;
        const auto preferred=f.state().choking.preferred;
        for (PeerId id=2; id<=7; ++id) {
            f.interest(id); f.interest(id);
            f.network.deliver(1,id,1,Message(MessageType::Bitfield,BitfieldPayload{{0x40}}));
            f.network.deliver(1,id,1,Message(MessageType::Have,HavePayload{1}));
        }
        f.interest(*optimistic,false); f.interest(*optimistic);
        check(f.state().choking.optimistic==optimistic && f.state().choking.preferred==preferred);
    });
    f.simulation.run();
    return trace;
}
void seededOptimisticReplay() {
    const auto baseline=optimisticTrace(42);
    check(baseline==optimisticTrace(42));
    check(baseline==optimisticTrace(42,true));
    check(baseline==optimisticTrace(42,false,true)); // Observations consume no draws.
    std::set<std::vector<PeerId>> traces;
    bool nonRoundRobin=false;
    for (std::uint64_t seed=0; seed<16; ++seed) {
        const auto trace=optimisticTrace(seed);
        traces.insert(trace);
        // After regular stabilization the pool is 5,6,7. A sorted rotation
        // would always move 5->6->7->5; random exploration must escape that order.
        for (std::size_t i=2; i<trace.size(); ++i) {
            check(trace[i]!=trace[i-1]);
            nonRoundRobin |= trace[i] != (trace[i-1]==7 ? 5u : trace[i-1]+1);
        }
    }
    check(traces.size()>1 && nonRoundRobin);
}
void optimisticEligibilityAndLowIdSeed() {
    for (const bool localSeed : {false,true}) {
        std::set<PeerId> winners;
        for (std::uint64_t seed=0; seed<64; ++seed) {
            Fixture f(localSeed,{},seed);
            // Remote 2 is a known, uninterested seed, not a preferred peer.
            f.state(2).localBitfield={0xc0};
            f.state().connections.at(2).remoteBitfield={0xc0};
            f.state(4).active=false;
            f.state().connections.at(5).handshakeReceived=false;
            f.interest(3); // Sole interested regular; candidates are 2,6,7.
            const auto chosen=f.state().choking.optimistic;
            check(chosen && (*chosen==2 || *chosen==6 || *chosen==7));
            check(f.state().choking.preferred==std::set<PeerId>{3});
            check(!f.state().connections.at(*chosen).remoteInterestedInUs);
            check(!f.state().connections.at(*chosen).weAreChokingRemote);
            for (PeerId id : {2u,6u,7u}) check(f.state().connections.at(id).optimisticConsidered);
            for (PeerId id : {3u,4u,5u}) check(!f.state().connections.at(id).optimisticConsidered);
            winners.insert(*chosen);
        }
        // Seeds remain eligible, but the smallest ID no longer wins every time.
        check(winners==std::set<PeerId>({2,6,7}));
    }
}
void newConnectionWeight() {
    unsigned newWins=0, oldWins=0;
    for (std::uint64_t seed=0; seed<1024; ++seed) {
        Fixture f(false,{},seed);
        for (PeerId id : {4u,5u,6u}) f.state().connections.at(id).handshakeReceived=false;
        f.state().connections.at(3).optimisticConsidered=true;
        f.interest(2); // Old 3 competes against new 7 with weights 1:3.
        const auto chosen=f.state().choking.optimistic;
        check(chosen==3 || chosen==7);
        newWins += chosen==7; oldWins += chosen==3;
        // Win or lose, the initial opportunity is consumed; ordinary interest
        // changes cannot restore the bonus or alter the chosen assignment.
        check(f.state().connections.at(3).optimisticConsidered);
        check(f.state().connections.at(7).optimisticConsidered);
        f.interest(7); f.interest(7,false);
        check(f.state().choking.optimistic==chosen);
        check(f.state().connections.at(7).optimisticConsidered);
    }
    // Fixed seed sweep, not a nondeterministic statistical/flaky test. Broad
    // bounds distinguish the documented 3:1 preference from uniform selection.
    check(newWins>2*oldWins && newWins<4*oldWins);
}
void newConnectionOpportunityAndReset() {
    Fixture f(false,{5,12}); f.presetOptimistic(3);
    for (PeerId id : {4u,5u,6u,7u}) f.state().connections.at(id).handshakeReceived=false;
    f.interest(2);
    f.at(11,[&] {
        // Complete a new connection between deadlines; it gains eligibility,
        // never an immediate assignment. Both endpoints already sent handshakes.
        f.network.deliver(1,7,1,Message(MessageType::Handshake,HandshakePayload{f.a.infoHash(),{}}));
        check(f.state().choking.optimistic==3);
        check(!f.state().connections.at(7).optimisticConsidered);
    });
    f.at(12.001,[&] {
        check(f.state().choking.optimistic==7); // Only alternative to the incumbent.
        check(f.state().connections.at(7).optimisticConsidered);
        check(!f.state().connections.at(4).optimisticConsidered);
        f.network.leaveSwarm(1,7);
        check(!f.state().connections.contains(7));
        JoinOptions options; options.numwant=0; options.targetOutgoingConnections=0;
        f.network.joinSwarm(1,7,options);
        f.network.send(1,7,1,Message(MessageType::Handshake,HandshakePayload{f.a.infoHash(),{}}));
    });
    f.at(12.1,[&] {
        check(f.state().connections.at(7).handshakeComplete());
        check(!f.state().connections.at(7).optimisticConsidered);
        // Finish this fixture before its synthetic incomplete peers receive stop traffic.
        f.network.leaveSwarm(1,7);
    });
    while (f.simulation.nextEventTime() && *f.simulation.nextEventTime()<=12.2) f.simulation.step();
}
void singletonAndEmptyOptimisticPools() {
    for (bool singleton : {false,true}) {
        Fixture f(false,{5,12});
        for (PeerId id=3; id<=7; ++id)
            if (!singleton || id!=7) f.state().connections.at(id).handshakeReceived=false;
        f.interest(2);
        check(f.state().choking.optimistic == (singleton ? std::optional<PeerId>{7} : std::nullopt));
        f.at(12.001,[&] {
            check(f.state().choking.optimistic == (singleton ? std::optional<PeerId>{7} : std::nullopt));
            check(f.state().choking.nextOptimisticDeadline==24);
            check(f.state().choking.nextRegularDeadline==15);
        });
        while (f.simulation.nextEventTime() && *f.simulation.nextEventTime()<=12.1) f.simulation.step();
    }
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
            if (start->message().type() == MessageType::Request) { ++requests; if (requests == 1) check(event.time() < 1); }
            if (start->message().type() == MessageType::Piece) ++pieces;
        }
    });
    for (PeerId id = 2; id <= count; ++id) network.send(1, id, 1,
        Message(MessageType::Handshake, HandshakePayload{swarm.infoHash(), peers[id - 1].protocolId()}));
    simulation.run();
    for (PeerId id = 1; id <= count; ++id) check(network.peer(id).swarmState(1).localBitfield[0] == 0x80);
    check(requests >= (count - 1) * 4 && pieces >= (count - 1) * 4);
    if (many) check(four && rotated && decisions >= 1);
}
}
int main() {
    const std::pair<const char*,std::function<void()>> tests[] = {
        {"Immediate bootstrap, interested budget and duplicate interest",bootstrap},
        {"NOT_INTERESTED defers refill until regular deadline",vacancies},
        {"Interested optimistic consumes a slot; uninterested optimistic does not",interestedSlotAccounting},
        {"Leecher pre-unchoke and reactive worst-peer replacement",[]{preUnchokedInterest(false);}},
        {"Seeder pre-unchoke and reactive worst-peer replacement",[]{preUnchokedInterest(true);}},
        {"Uninterested optimistic rotation preserves four regular assignments",[]{uninterestedRotation(false);}},
        {"Interested optimistic rotation only evicts the necessary worst regular",[]{uninterestedRotation(true);}},
        {"Local non-round 5/25 deadlines",[]{deadlines(5,25);}},
        {"Independent 10/25 deadlines",[]{deadlines(10,25);}},
        {"Equal 5/5 deadlines",[]{deadlines(5,5);}},
        {"Optimistic-only rotation preserves regular measurements",optimisticOnly},
        {"Leecher TFT ranking preserves an eligible optimistic peer",[]{ranking(false);}},
        {"Seeder upload ranking preserves an eligible optimistic peer",[]{ranking(true);}},
        {"Rolling history outranks a latest-interval burst",rollingBurstRanking},
        {"Combined deadline promotes before optimistic selection without wire churn",combinedPromotion},
        {"Configured leecher rolling buckets",[]{rollingRates(false);}},
        {"Configured seeder rolling buckets",[]{rollingRates(true);}},
        {"Interest changes and duplicate wake-ups preserve clock and history",stableClockAndDuplicates},
        {"HAVE/BITFIELD/PIECE and duplicate observations never reselect",observationsDoNotSelect},
        {"Idle periods preserve phase and age payload at original boundaries",idlePhaseAndAccounting},
        {"Departure repairs only the missing optimistic assignment",departureRepairOnly},
        {"Useful bytes exclude overlap and control traffic",usefulOnly},
        {"Swarm-local cycles",isolation},
        {"Configuration and deadline validation",validation},
        {"Seeded optimistic replay, varied seeds, stable enumeration and no ID rotation",seededOptimisticReplay},
        {"Optimistic eligibility includes seeds without lowest-ID priority",optimisticEligibilityAndLowIdSeed},
        {"New connections have a threefold initial selection preference",newConnectionWeight},
        {"New-connection opportunity waits for a decision and resets on reconnect",newConnectionOpportunityAndReset},
        {"Singleton and empty optimistic candidate pools",singletonAndEmptyOptimisticPools},
        {"Immediate real request pipeline",[]{realPipeline(false);}},
        {"Seven-peer choking integration",[]{realPipeline(true);}}
    };
    unsigned failed=0;
    for(const auto& [name,test]:tests) {
        try{test();std::cout<<"PASS: "<<name<<std::endl;}
        catch(const std::exception& e){++failed;std::cerr<<"FAIL: "<<name<<": "<<e.what()<<std::endl;}
    }
    return failed?1:0;
}
