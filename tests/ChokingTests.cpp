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
    explicit Fixture(bool isSeed = false, BitTorrentSettings settings = {})
        : network(simulation, peers(isSeed),
            {Link(1,2,1e7,.01), Link(1,3,1e7,.01), Link(1,4,1e7,.01),
             Link(1,5,1e7,.01), Link(1,6,1e7,.01), Link(1,7,1e7,.01)}, {a,b}, 50, 1800, settings),
          seed(isSeed) {
        at(81, [&] { stop(); });
    }
    PeerSwarmState& state(PeerId id = 1, SwarmId swarm = 1) {
        return const_cast<PeerSwarmState&>(network.peer(id).swarmState(swarm));
    }
    void at(double t, std::function<void()> f) { simulation.schedule(std::make_unique<Action>(t, std::move(f))); }
    void interest(PeerId id, bool interested = true, SwarmId swarm = 1) {
        network.deliver(swarm, id, 1, Message(interested ? MessageType::Interested : MessageType::NotInterested));
    }
    void all(SwarmId swarm = 1) { for (PeerId id = 2; id <= 7; ++id) interest(id, true, swarm); }
    void stop() {
        for (SwarmId swarm : {1u,2u}) for (PeerId id=2; id<=7; ++id) {
            interest(id, false, swarm);
            network.deliver(swarm, 1, id, Message(MessageType::NotInterested));
        }
    }
    void invariants(SwarmId swarm = 1) {
        const auto& s = state(1, swarm);
        check(s.choking.preferred.size() <= 3);
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
    for (PeerId id=2; id<=6; ++id) f.at(2.347 + (id-2)*.003, [&,id] {
        f.interest(id);
        f.interest(id); // Duplicate must not reset the cycle or repeat wire transitions.
        f.invariants();
        check(near(f.state().choking.nextRegularDeadline,12.347));
        check(near(f.state().choking.nextOptimisticDeadline,32.347));
        check(f.state().choking.preferred.size()==std::min<std::size_t>(id-1,3));
        if (id==6) check(f.state().connections.at(6).weAreChokingRemote);
    });
    f.at(2.4,[&] { check(unchokes==4 && regular==0); check(f.state().choking.optimistic==5); });
    f.at(12.348,[&] { check(regular==1); check(f.state().choking.preferred==std::set<PeerId>({2,3,4})); });
    f.simulation.run();
}
void vacancies() {
    Fixture f; f.all();
    f.at(3,[&] {
        f.state().connections.at(2).downloadedInWindow=123;
        f.interest(2,false);
        check(f.state().connections.at(2).weAreChokingRemote);
        check(f.state().choking.preferred==std::set<PeerId>({3,4,5}));
        check(f.state().choking.optimistic==6);
        check(f.state().connections.at(2).downloadedInWindow==123);
        check(f.state().choking.nextRegularDeadline==10 && f.state().choking.nextOptimisticDeadline==30);
        f.interest(2); // Incumbents stay put.
        check(f.state().connections.at(2).weAreChokingRemote);
        f.invariants();
    });
    f.simulation.run();
}
void singleAndNoBorrowing() {
    Fixture f;
    f.interest(2);
    check(!f.state().connections.at(2).weAreChokingRemote);
    check(f.state().choking.preferred==std::set<PeerId>({2}) && !f.state().choking.optimistic);
    f.interest(3); f.interest(4);
    check(f.state().choking.preferred.size()==3 && !f.state().choking.optimistic);
    f.interest(5);
    check(f.state().choking.preferred.size()==3 && f.state().choking.optimistic==5);
    f.interest(5,false);
    check(f.state().choking.preferred.size()==3 && !f.state().choking.optimistic);
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
    f.at(24,[&]{f.state().connections.at(2).downloadedInWindow=99;});
    f.at(25.001,[&] {
        check(f.state().choking.preferred==std::set<PeerId>({2,3,4}));
        check(f.state().choking.optimistic==6);
        check(f.state().connections.at(2).downloadedInWindow==99);
        check(f.state().choking.windowStart==20 && f.state().choking.nextRegularDeadline==30);
    });
    f.at(50.001,[&]{check(f.state().choking.optimistic==7); f.invariants();});
    f.simulation.run();
}
void ranking(bool seed) {
    Fixture f(seed,{5,25}); f.all();
    for(PeerId id=2;id<=7;++id)
        f.payload(seed?1:id,seed?id:1,(id-2)*1000,(id-1)*100);
    f.at(5.001,[&] {
        check(f.state().choking.preferred==std::set<PeerId>({5,6,7}));
        check(f.state().choking.optimistic==2); // Former optimistic 5 promoted, wrap cursor.
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
    Fixture f(false,{5,5}); f.all();
    unsigned optimisticWireTransitions=0;
    f.simulation.setEventObserver([&](const Event& e){
        if(const auto* s=dynamic_cast<const SendMessageEvent*>(&e);
            s && e.time()==5 && s->details().sender==1 && s->details().receiver==5
            && (s->details().messageType==MessageType::Choke || s->details().messageType==MessageType::Unchoke))
            ++optimisticWireTransitions;
    });
    f.at(4,[&]{f.state().connections.at(5).downloadedInWindow=1000;});
    f.at(5.001,[&]{
        check(f.state().choking.preferred==std::set<PeerId>({2,3,5}));
        check(f.state().choking.optimistic==6);
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
void restartAndDuplicates() {
    Fixture f(false,{5,25});
    f.interest(2);
    const auto first=f.state().choking.cycleGeneration;
    // A retired-cycle event with the NEW cycle's exact deadline must still be
    // rejected by its token, before consuming the new pending wake-up.
    f.simulation.schedule(std::make_unique<RechokeEvent>(2.347 + 5,f.network,1,1));
    f.at(2.347 + 5,[&]{
        check(!f.state().choking.previousIntervalMeasured);
        check(f.state().choking.eventPending);
    });
    // The old cycle's ordinary pending event will also become stale.
    f.simulation.schedule(std::make_unique<RechokeEvent>(5,f.network,1,1));
    f.at(1,[&]{
        f.state().connections.at(2).downloadedInWindow=900;
        f.interest(2,false);
        check(!f.state().choking.cycleActive && !f.state().choking.eventPending);
    });
    f.at(2.347,[&]{
        f.interest(2);
        check(f.state().choking.cycleGeneration!=first);
        check(f.state().connections.at(2).downloadedInWindow==0);
        check(near(f.state().choking.nextRegularDeadline,7.347));
        f.simulation.schedule(std::make_unique<RechokeEvent>(2.347 + 5,f.network,1,1));
    });
    f.at(5.001,[&]{
        check(f.state().choking.eventPending);
        check(near(*f.state().choking.pendingWakeup,7.347));
        check(!f.state().choking.previousIntervalMeasured);
    });
    f.at(7.348,[&]{
        check(f.state().choking.previousIntervalMeasured);
        check(near(*f.state().choking.pendingWakeup,12.347));
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
        {"Immediate bootstrap, strict 3+1 and duplicate interest",bootstrap},
        {"Vacancy filling preserves deadlines and buckets",vacancies},
        {"Single peer and no preferred borrowing",singleAndNoBorrowing},
        {"Local non-round 5/25 deadlines",[]{deadlines(5,25);}},
        {"Independent 10/25 deadlines",[]{deadlines(10,25);}},
        {"Equal 5/5 deadlines",[]{deadlines(5,5);}},
        {"Optimistic-only rotation preserves regular measurements",optimisticOnly},
        {"Leecher TFT ranking and optimistic promotion",[]{ranking(false);}},
        {"Seeder upload ranking and optimistic promotion",[]{ranking(true);}},
        {"Rolling history outranks a latest-interval burst",rollingBurstRanking},
        {"Combined deadline promotes before optimistic selection without wire churn",combinedPromotion},
        {"Configured leecher rolling buckets",[]{rollingRates(false);}},
        {"Configured seeder rolling buckets",[]{rollingRates(true);}},
        {"Idle restart and duplicate/stale wake-ups",restartAndDuplicates},
        {"Useful bytes exclude overlap and control traffic",usefulOnly},
        {"Swarm-local cycles",isolation},
        {"Configuration and deadline validation",validation},
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
