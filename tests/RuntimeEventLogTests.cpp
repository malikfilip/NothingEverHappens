#include "RuntimeEventLog.hpp"
#include "RuntimeLogDialog.hpp"
#include "RuntimeSession.hpp"
#include "simulator/TransmissionStartEvent.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
#include "simulator/MessageArrivalEvent.hpp"
#include <QApplication>
#include <QLabel>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace simulator;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    std::vector<ScenarioSwarm> scenario;
    std::unique_ptr<RuntimeSession> runtime;
    RuntimeEventLog log;
    std::set<std::pair<double, QString>> observed;
    Fixture() {
        ScenarioSwarm swarm;
        swarm.id = 1; swarm.name = "Test";
        swarm.totalSizeBytes = 16384; swarm.pieceSizeBytes = 4096; swarm.pieceCount = 4;
        for (quint64 id = 1; id <= 3; ++id) {
            ScenarioPeer peer;
            peer.id = id; peer.name = id == 1 ? "SEED" : "Potato";
            peer.initiallyJoined = true;
            peer.initialRole = id == 1 ? ScenarioPeer::Role::Seeder : ScenarioPeer::Role::Leecher;
            peer.initialPieceCount = id == 1 ? 4 : 0;
            peer.uploadBytesPerSecond = peer.downloadBytesPerSecond = 128;
            peer.position = {double(id) * 100, 0};
            swarm.peers.push_back(peer);
        }
        scenario.push_back(swarm);
        runtime = RuntimeSession::create(scenario, {0,0,400,400});
        log.peerName = [](PeerId id) { return id == 1 ? "SEED" : "Potato"; };
        log.swarmName = [](SwarmId) { return "Test"; };
        runtime->simulation().setEventObserver([this](const Event& event) {
            if (dynamic_cast<const TransmissionStartEvent*>(&event)) observed.emplace(event.time(), "SEND");
            if (dynamic_cast<const MessageArrivalEvent*>(&event)) observed.emplace(event.time(), "RECEIVE");
            log.observe(event, runtime->network());
        });
    }
    bool step(bool paused = false) {
        const bool result = runtime->simulation().step();
        log.finishStep(true, paused);
        return result;
    }
    void membership(bool active) {
        runtime->setPeerMembership(1, 2, active);
        log.finishStep(true, true);
    }
};
void scopedMessagesAndLifecycle()
{
    Fixture f;
    f.log.visible = [](MessageType type) { return type != MessageType::Request; };
    for (unsigned i = 0; i < 5; ++i) f.step();
    f.log.flush();
    check(f.log.rowCount() == 0, "No selected peer must mean no history");
    f.log.select(RuntimeEventLog::Selection{2,1});
    check(f.log.rowCount() == 0, "Selection must not reconstruct history");
    unsigned steps = 0;
    while (f.runtime->simulation().nextEventTime() && *f.runtime->simulation().nextEventTime() < 600) {
        check(++steps < 20000, "Bounded event scenario");
        f.step();
    }
    check(f.log.rowCount() == 0, "Running rows must be batched without per-event model updates");
    f.log.flush();
    bool send = false, receive = false, piece = false, complete = false;
    int messageRow = -1, highLevelRow = -1;
    for (int i = 0; i < f.log.rowCount(); ++i) {
        const auto& row = *f.log.entry(i);
        check(row.sender == 2 || row.receiver == 2, "Unselected peer leaked into Event Log");
        if (row.message) {
            check(row.message->type() != MessageType::Request, "Message Filter ignored");
            check(row.action == "SEND" || row.action == "RECEIVE", "Internal transmission event leaked");
            check(f.observed.contains({row.time, row.action}), "Timestamp must come from real start/arrival");
            check(f.log.inspectionText(row).contains("Wire size:") && f.log.inspectionText(row).contains("Test"),
                "Message snapshot lacks common inspection fields");
            send |= row.action == "SEND"; receive |= row.action == "RECEIVE";
            messageRow = i;
        } else {


            piece |= row.action.startsWith("PIECE COMPLETED");
            complete |= row.action.startsWith("DOWNLOAD COMPLETED");
            highLevelRow = i;
        }
    }
    check(send && receive, "Missing SEND/RECEIVE");


    check(piece, "Missing piece completion");
    check(complete, "Missing download completion");
    check(!createRuntimeLogDialog(f.log, messageRow, false), "Running inspection must be disabled");
    check(!createRuntimeLogDialog(f.log, highLevelRow, true), "High-level row should not create a message popup");
    auto dialog = createRuntimeLogDialog(f.log, messageRow, true);
    check(dialog != nullptr, "Paused message row must open");
    const auto* details = dialog->findChild<QLabel*>("messageLogDetails");
    check(details && details->text() == f.log.inspectionText(*f.log.entry(messageRow)), "Popup must show saved modeled message");
    f.log.select(RuntimeEventLog::Selection{3,1});
    check(f.log.rowCount() == 0, "Peer switch must clear immediately");
    f.log.flush();
    check(f.log.rowCount() == 0, "Peer switch retained a pending batch");
    f.log.select(std::nullopt);
    f.step(true);
    check(f.log.rowCount() == 0, "Deselection must remain empty");
    f.log.select(RuntimeEventLog::Selection{2,1});
    f.log.visible = [](MessageType) { return false; };
    f.membership(false);
    check(f.log.rowCount() == 1 && f.log.entry(0)->action == "LEFT SWARM", "Leave independent of filter");
    f.membership(false);
    check(f.log.rowCount() == 1, "No-op leave must not log");
    f.membership(true);
    check(f.log.rowCount() == 2 && f.log.entry(1)->action == "JOINED SWARM", "Join independent of filter");
    f.log.resetSession();
    check(f.log.rowCount() == 0, "Session teardown retained rows");
}
void activeMessagePopup()
{
    ActiveTransmission active{7, 1, 2, 3, Message(MessageType::Request, RequestPayload{4, 128, 256}),
        0, 80, 16, 2, 0, {}};
    check(!createActiveTransmissionDialog(active, 4, false), "Running link inspection must match log popup behavior");
    auto dialog = createActiveTransmissionDialog(active, 4, true);
    check(dialog != nullptr, "Paused link message must open a popup");
    const auto* details = dialog->findChild<QLabel*>("messageLogDetails");
    check(details && details->text().contains("Remaining: 48 bits")
        && details->text().contains("Index: 4") && details->text().contains("Length: 256 bytes"),
        "Active popup must preserve transmission details and actual message fields");
}
void startupMembership()
{
    Fixture f;
    f.log.select(RuntimeEventLog::Selection{2,1});
    f.log.sessionStarted(f.runtime->network(), f.runtime->simulation().currentTime());
    check(f.log.rowCount() == 1 && f.log.entry(0)->action == "JOINED SWARM" && f.log.entry(0)->time == 0,
        "Selected peer's real startup join must be logged");
    f.log.select(RuntimeEventLog::Selection{3,1});
    check(f.log.rowCount() == 0, "Selecting another already-joined peer must not reconstruct its join");
}
void selectedSeedRechokes()
{
    Fixture f;
    f.log.select(RuntimeEventLog::Selection{1,1});
    f.log.visible = [](MessageType) { return false; };
    unsigned steps = 0;
    while (f.runtime->simulation().nextEventTime() && *f.runtime->simulation().nextEventTime() < 65) {
        check(++steps < 5000, "Bounded seed rechoke scenario");
        f.step();
    }
    f.log.flush();
    bool regular = false, optimistic = false;
    for (int i = 0; i < f.log.rowCount(); ++i) {
        const auto& row = *f.log.entry(i);
        check(row.sender == 1 && !row.message, "Seed selection/filter isolation");
        regular |= row.action == "REGULAR RECHOKE";
        optimistic |= row.action == "OPTIMISTIC RECHOKE";
    }
    check(regular && optimistic, "Missing actual seed rechokes with all message types disabled");
}
void staleArrivalsAndSelectionDuringPropagation()
{
    Fixture f;
    bool completion = false;
    double arrivalTime = 0;
    MessageType type = MessageType::Handshake;
    f.runtime->simulation().setEventObserver([&](const Event& event) {
        if (const auto* done = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
            const auto found = f.runtime->network().activeTransmissions().find(done->transmissionId());
            if (!completion && found != f.runtime->network().activeTransmissions().end()
                && found->second.generation == done->generation() && found->second.receiver == 2) {
                completion = true;
                arrivalTime = event.time() + f.runtime->network().links()[found->second.linkIndex].latency();
                type = found->second.message.type();
            }
        }
        f.log.observe(event, f.runtime->network());
    });
    for (unsigned i = 0; !completion && i < 1000; ++i) f.step();
    check(completion, "No real in-flight message");
    f.log.select(RuntimeEventLog::Selection{2,1});
    while (f.runtime->simulation().nextEventTime() && *f.runtime->simulation().nextEventTime() <= arrivalTime) f.step(true);
    bool received = false;
    for (int i = 0; i < f.log.rowCount(); ++i) {
        const auto& row = *f.log.entry(i);
        received |= row.action == "RECEIVE" && row.time == arrivalTime && row.message->type() == type;
    }
    check(received, "A real arrival after selection must retain its actual payload without reconstructing history");

    Fixture stale;
    stale.log.select(RuntimeEventLog::Selection{2,1});
    bool ready = false;
    double deadline = 0;
    stale.runtime->simulation().setEventObserver([&](const Event& event) {
        if (const auto* done = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
            const auto found = stale.runtime->network().activeTransmissions().find(done->transmissionId());
            if (!ready && found != stale.runtime->network().activeTransmissions().end()
                && found->second.receiver == 2) {
                ready = true;
                deadline = event.time() + stale.runtime->network().links()[found->second.linkIndex].latency();
            }
        }
        stale.log.observe(event, stale.runtime->network());
    });
    for (unsigned i = 0; !ready && i < 1000; ++i) stale.step();
    check(ready, "No stale-arrival setup");
    stale.membership(false);
    const auto count = stale.log.rowCount();
    while (stale.runtime->simulation().nextEventTime() && *stale.runtime->simulation().nextEventTime() <= deadline) stale.step(true);
    check(stale.log.rowCount() == count, "Departed peer's stale arrival must not log RECEIVE");
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try { activeMessagePopup(); startupMembership(); scopedMessagesAndLifecycle(); selectedSeedRechokes(); staleArrivalsAndSelectionDuringPropagation(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Runtime Event Log tests passed\n";
}
