#include "RuntimeSession.hpp"
#include "ScenarioPieces.hpp"
#include "PeerInspection.hpp"
#include "simulator/TrackerAnnounceEvent.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(double a, double b) { return std::abs(a - b) < 1e-10; }

ScenarioPeer peer(quint64 id, bool joined, ScenarioPeer::Role role, quint64 count, QPointF position = {})
{
    ScenarioPeer result;
    result.id = id;
    result.name = QStringLiteral("Same peer name");
    result.initiallyJoined = joined;
    result.initialRole = role;
    result.initialPieceCount = count;
    result.uploadBytesPerSecond = 10 * 1024 * 1024;
    result.downloadBytesPerSecond = 20 * 1024 * 1024;
    result.position = position;
    return result;
}
ScenarioSwarm swarm(quint64 id, quint64 count = 10)
{
    ScenarioSwarm result;
    result.id = id;
    result.name = QStringLiteral("Same swarm name");
    result.mode = ScenarioSwarm::Mode::Virtual;
    result.totalSizeBytes = count * 1024;
    result.pieceSizeBytes = 1024;
    result.pieceCount = count;
    return result;
}
std::vector<ScenarioSwarm> scenario()
{
    auto a = swarm(5000000001), b = swarm(5000000002), skipped = swarm(5000000003);
    a.peers = {peer(9000000001, true, ScenarioPeer::Role::Seeder, 10, {0, 0}),
        peer(9000000002, true, ScenarioPeer::Role::Leecher, 3, {100, 100}),
        peer(9000000003, false, ScenarioPeer::Role::Leecher, 2, {50, 50})};
    b.peers = {peer(9000000004, true, ScenarioPeer::Role::Seeder, 10),
        peer(9000000005, false, ScenarioPeer::Role::Leecher, 0)};
    skipped.peers = {peer(9000000006, false, ScenarioPeer::Role::Leecher, 0)};
    return {a, b, skipped};
}
quint64 countBits(const std::vector<std::uint8_t>& bits)
{
    quint64 count = 0;
    for (auto byte : bits) count += std::popcount(byte);
    return count;
}
void preflight()
{
    auto input = scenario();
    auto result = RuntimeSession::preflight(input);
    check(result.errors.empty() && result.participatingSwarms.size() == 2, "Global participation");
    check(result.singlePeerSwarms.size() == 1 && result.noLeecherSwarms.size() == 1
        && result.skippedSwarms.size() == 1, "Warning groups incorrect");
    for (const auto& s : input) for (const auto& p : s.peers)
        check(!p.initialBitfield, "Preflight mutated scenario before confirmation");
    input.resize(1);
    result = RuntimeSession::preflight(input);
    check(result.errors.empty() && !result.hasWarnings(), "Healthy swarm should start directly");
    input[0].peers = {peer(1, true, ScenarioPeer::Role::Leecher, 0)};
    result = RuntimeSession::preflight(input);
    check(result.singlePeerSwarms.size() == 1 && result.noLeecherSwarms.empty(), "Single leecher warning");
    input[0].peers[0].initiallyJoined = false;
    result = RuntimeSession::preflight(input);
    check(!result.errors.empty() && result.noLeecherSwarms.empty(), "Empty swarm falsely warned about leechers");
    check(!RuntimeSession::preflight({}).errors.empty(), "Empty scenario accepted");
}
void rejectsWithoutMutation(std::vector<ScenarioSwarm> input, QRectF rect = {0, 0, 100, 100})
{
    const auto before = input;
    bool rejected = false;
    try { auto session = RuntimeSession::create(input, rect, 7); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Invalid configuration accepted");
    for (std::size_t i = 0; i < input.size(); ++i)
        for (std::size_t j = 0; j < input[i].peers.size(); ++j)
            check(input[i].peers[j].initialBitfield == before[i].peers[j].initialBitfield
                && input[i].peers[j].initialPieceCount == before[i].peers[j].initialPieceCount,
                "Failed startup partially published ownership");
}
void validationAndTransaction()
{
    auto input = scenario();
    input[1].pieceSizeBytes = quint64(std::numeric_limits<std::uint32_t>::max()) + 1;
    rejectsWithoutMutation(input);
    input = scenario(); input[1].totalSizeBytes = std::numeric_limits<quint64>::max(); rejectsWithoutMutation(input);
    input = scenario(); input[0].pieceCount = 9; rejectsWithoutMutation(input);
    input = scenario(); input[0].peers[2].uploadBytesPerSecond = 0; rejectsWithoutMutation(input);
    input = scenario(); input[0].peers[1].initialPieceCount = 11; rejectsWithoutMutation(input);
    input = scenario(); input[1].id = input[0].id; rejectsWithoutMutation(input);
    input = scenario(); input[1].peers[0].id = input[0].peers[0].id; rejectsWithoutMutation(input);
    input = scenario(); input[0].peers[1].initialBitfield = std::vector<std::uint8_t>{0x80}; rejectsWithoutMutation(input);
    input = scenario(); input[0].peers[1].initialBitfield = std::vector<std::uint8_t>{0xc0, 0x01}; rejectsWithoutMutation(input);
    input = scenario(); input[0].peers[1].initialBitfield = std::vector<std::uint8_t>{0x80, 0}; rejectsWithoutMutation(input);
    input = scenario(); input[0].peers[0].initialBitfield = std::vector<std::uint8_t>{0, 0}; rejectsWithoutMutation(input);
    input = scenario(); input[0].peers[0].position.setX(std::numeric_limits<double>::quiet_NaN()); rejectsWithoutMutation(input);
    rejectsWithoutMutation(scenario(), {0, 0, 0, 100});
}
void materializationAndLifecycle()
{
    auto input = scenario();
    auto session = RuntimeSession::create(input, {0, 0, 100, 100}, 1234);
    const auto& net = session->network();
    check(net.swarms().size() == 2 && net.peers().size() == 5, "Participating objects not materialized globally");
    check(!session->swarmIds().contains(input[2].id), "Skipped swarm materialized");
    check(!input[2].peers[0].initialBitfield, "Skipped swarm ownership generated");
    const auto a = session->swarmIds().at(input[0].id), b = session->swarmIds().at(input[1].id);
    check(net.swarm(a).infoHash() != net.swarm(b).infoHash(), "Duplicate-name swarms share identity");
    check(net.swarm(a).totalSize() == 10240 && net.swarm(a).pieceLength() == 1024
        && net.swarm(a).pieceCount() == 10, "Rich swarm metadata lost");
    std::set<simulator::PeerProtocolId> protocols;
    for (std::size_t i = 0; i < 2; ++i) for (const auto& p : input[i].peers) {
        const auto& binding = session->peerBindings().at(p.id);
        const auto& enginePeer = net.peer(binding.peerId);
        check(binding.scenarioSwarmId == input[i].id && binding.swarmId == session->swarmIds().at(input[i].id), "Membership mapping");
        check(protocols.insert(enginePeer.protocolId()).second, "Protocol peer identity duplicated");
        check(enginePeer.uploadCapacity() == double(p.uploadBytesPerSecond) * 8
            && enginePeer.downloadCapacity() == double(p.downloadBytesPerSecond) * 8, "Byte/bit rate conversion");
        check(p.initialBitfield && binding.initialBitfield == *p.initialBitfield, "Inventory not preserved for future joins");
        check(countBits(binding.initialBitfield) == p.initialPieceCount, "Initial ownership count");
        if (p.initiallyJoined) {
            check(enginePeer.isActiveInSwarm(binding.swarmId), "Initially joined peer inactive");
            check(enginePeer.swarmState(binding.swarmId).localBitfield == *p.initialBitfield, "Join inventory differs");
            check(net.lifecycleGeneration(binding.swarmId, binding.peerId) == 1, "Lifecycle not entered normally");
        } else check(!enginePeer.hasSwarm(binding.swarmId), "Red peer entered membership");
    }
    check(net.links().empty() && net.tracker().registeredPeers(net.swarm(a).infoHash()).empty()
        && net.tracker().registeredPeers(net.swarm(b).infoHash()).empty(), "Startup executed pending events");
    check(session->simulation().currentTime() == 0, "Startup advanced time");
    int started = 0;
    session->simulation().setEventObserver([&](const simulator::Event& event) {
        const auto* announce = dynamic_cast<const simulator::TrackerAnnounceEvent*>(&event);
        check(announce && announce->kind() == simulator::AnnounceKind::Started && event.time() == 0,
            "Initial event was not the real t=0 STARTED lifecycle event");
        ++started;
    });
    // Mutations after construction must not influence frozen links or engine state.
    input[0].peers[0].position = {90, 90};
    input[0].trackerPosition = {1e9, -1e9};
    for (int i = 0; i < 3; ++i) check(session->simulation().step(), "Missing STARTED event");
    check(started == 3, "Unexpected startup announces");
    session->simulation().setEventObserver({});
    check(net.tracker().registeredPeers(net.swarm(a).infoHash()).size() == 2
        && net.tracker().registeredPeers(net.swarm(b).infoHash()).size() == 1, "Tracker isolation/registration");
    const auto seed = session->peerBindings().at(input[0].peers[0].id).peerId;
    const auto leech = session->peerBindings().at(input[0].peers[1].id).peerId;
    const auto* link = net.link(seed, leech);
    check(net.links().size() == 1 && link && near(link->latency(), .05)
        && link->bandwidth() == 100.0 * 1024 * 1024 * 8, "Lazy frozen link configuration");
}
void blockConfigurationAndInspection()
{
    auto input = scenario();
    check(input[0].blockSizeBytes == 16384, "Scenario default block changed");
    input[0].blockSizeBytes = 0;
    rejectsWithoutMutation(input);
    input[0].blockSizeBytes = 2048;
    rejectsWithoutMutation(input);
    input[0].blockSizeBytes = quint64(std::numeric_limits<std::uint32_t>::max()) + 1;
    rejectsWithoutMutation(input);
    input = scenario();
    input[0].blockSizeBytes = 512;
    // Prepare exactly once during editing, with the same helper used by startup.
    for (auto& s : input) for (auto& p : s.peers) ensureScenarioPieces(s, p, 123);
    const auto initial = *input[0].peers[1].initialBitfield;
    const auto edit = inspectPeer(input[0], input[0].peers[1], nullptr);
    check(!edit.runtime && edit.joined && edit.ownedPieces == 3 && !edit.complete()
        && edit.pieces == initial, "EDIT inspection did not use retained ownership");
    ensureScenarioPieces(input[0], input[0].peers[1], 999);
    check(input[0].peers[1].initialBitfield == initial, "Prepared pieces were randomized again");
    auto session = RuntimeSession::create(input, {0, 0, 100, 100}, 999);
    check(session->network().swarm(session->swarmIds().at(input[0].id)).blockSize() == 512,
        "Bridge lost custom block size");
    check(session->network().swarm(session->swarmIds().at(input[1].id)).blockSize() == 16384,
        "One swarm's custom block size affected another swarm");
    const auto inactive = inspectPeer(input[0], input[0].peers[2], session.get());
    check(!inactive.joined && inactive.enginePeerId && inactive.ownedPieces == 2
        && inactive.pieces == input[0].peers[2].initialBitfield, "Never-joined inspection lost inventory");
    const auto skipped = inspectPeer(input[2], input[2].peers[0], session.get());
    check(!skipped.joined && !skipped.enginePeerId, "Skipped swarm acquired engine identity");
    const auto engineId = session->peerBindings().at(input[0].peers[1].id).peerId;
    const auto swarmId = session->swarmIds().at(input[0].id);
    // Corrupt only the frozen scenario presentation values after startup. Runtime
    // inspection must still derive ownership, role, joined state and rates from engine.
    input[0].peers[1].initialRole = ScenarioPeer::Role::Seeder;
    input[0].peers[1].initiallyJoined = false;
    input[0].peers[1].initialPieceCount = 10;
    input[0].peers[1].uploadBytesPerSecond = 1;
    input[0].peers[1].initialBitfield = std::vector<std::uint8_t>{0xff, 0xc0};
    bool progressed = false, completed = false, connected = false, pending = false;
    quint64 previous = 3;
    for (int steps = 0; steps < 10000 && session->simulation().step(); ++steps) {
        const auto state = inspectPeer(input[0], input[0].peers[1], session.get());
        const auto& engine = session->network().peer(engineId);
        check(state.joined && state.enginePeerId == engineId
            && state.uploadBytesPerSecond == engine.uploadCapacity() / 8
            && state.pieces == engine.swarmState(swarmId).localBitfield,
            "Runtime Inspector used stale scenario state");
        check(state.ownedPieces >= previous, "Piece count regressed");
        progressed |= state.ownedPieces > previous;
        previous = state.ownedPieces;
        connected |= state.established > 0;
        pending |= state.outstanding + state.reserved > 0;
        if (state.complete()) { completed = true; break; }
    }
    check(progressed && completed && connected && pending, "Post-step Inspector did not follow download to completion");
    check(session->network().links().size() == 1, "Inspection changed lazy link topology");
}

void deterministicOwnershipAndGeometry()
{
    auto original = scenario(), first = original, second = original;
    auto a = RuntimeSession::create(first, {0, 0, 100, 100}, 123);
    std::reverse(second.begin(), second.end());
    for (auto& s : second) {
        std::reverse(s.peers.begin(), s.peers.end());
        for (auto& p : s.peers) p.position = p.position * 2 + QPointF(20, 30);
        s.trackerPosition = {900, -500};
    }
    auto b = RuntimeSession::create(second, {20, 30, 200, 200}, 123);
    for (const auto& [id, binding] : a->peerBindings()) {
        const auto& other = b->peerBindings().at(id);
        check(binding.initialBitfield == other.initialBitfield, "Ownership depends on iteration order");
        check(binding.normalizedPosition == other.normalizedPosition, "Geometry depends on pixel size/origin");
    }
    for (const auto& [id, engineId] : a->swarmIds())
        check(a->network().swarm(engineId).infoHash() == b->network().swarm(b->swarmIds().at(id)).infoHash(),
            "Synthetic hash depends on engine enumeration");
    auto stable = RuntimeSession::create(first, {0, 0, 100, 100}, 999);
    for (const auto& [id, binding] : a->peerBindings())
        check(binding.initialBitfield == stable->peerBindings().at(id).initialBitfield, "Concrete ownership rerandomized");

    bool nonPrefix = false, seedChanges = false;
    for (quint64 k = 0; k <= 65; ++k) {
        auto s = swarm(1, 65);
        s.peers = {peer(1, true, ScenarioPeer::Role::Leecher, k)};
        std::vector<ScenarioSwarm> one{s}, two{s};
        auto c = RuntimeSession::create(one, {0, 0, 100, 100}, 42);
        auto d = RuntimeSession::create(two, {0, 0, 100, 100}, 43);
        const auto& bits = *one[0].peers[0].initialBitfield;
        check(countBits(bits) == k && bits.size() == 9 && (bits.back() & 0x7f) == 0, "Sampling count/tail bits");
        std::vector<std::uint8_t> prefix(9, 0);
        for (quint64 i = 0; i < k; ++i) prefix[i / 8] |= 0x80u >> (i % 8);
        nonPrefix |= bits != prefix;
        seedChanges |= bits != *two[0].peers[0].initialBitfield;
    }
    check(nonPrefix && seedChanges, "Piece sampling ignores seeded randomness");
}
}

int main()
{
    try {
        preflight();
        validationAndTransaction();
        materializationAndLifecycle();
        deterministicOwnershipAndGeometry();
        blockConfigurationAndInspection();
        std::cout << "Runtime session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
