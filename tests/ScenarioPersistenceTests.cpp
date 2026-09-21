#include "ScenarioPersistence.hpp"
#include "RuntimeSession.hpp"
#include "PeerInspection.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QFile>
#include <iostream>
#include <limits>
#include <functional>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
ScenarioProject fixture() {
    ScenarioProject project;
    project.seed = std::numeric_limits<quint64>::max();
    project.settings.bitTorrent = {5, 12.5};
    ScenarioSwarm s;
    s.id = 9007199254740993ULL;
    s.name = QString::fromUtf8("Virtual swarm – exact");
    s.mode = ScenarioSwarm::Mode::Virtual;
    s.totalSizeBytes = 6144;
    s.pieceSizeBytes = 1024;
    s.pieceCount = 6;
    s.blockSizeBytes = 256;
    s.trackerPosition = {-50.125, 60.75};
    s.virtualSizeDisplayValue = "6"; s.virtualSizeDisplayUnit = "KiB";
    s.pieceSizeDisplayValue = "1"; s.pieceSizeDisplayUnit = "KiB";
    s.blockSizeDisplayValue = "256"; s.blockSizeDisplayUnit = "B";
    ScenarioPeer p;
    p.id = 9007199254740995ULL;
    p.name = "Seeder";
    p.initialRole = ScenarioPeer::Role::Seeder;
    p.initiallyJoined = true;
    p.initialPieceCount = 6;
    p.initialBitfield = std::vector<std::uint8_t>{0xfc};
    p.uploadBytesPerSecond = 65536; p.downloadBytesPerSecond = 131072;
    p.uploadDisplayValue = "64"; p.uploadDisplayUnit = "KiB/s";
    p.downloadDisplayValue = "128"; p.downloadDisplayUnit = "KiB/s";
    p.position = {-20.5, -30.25};
    s.peers.push_back(p);
    p.id = 42; p.name = "Leecher";
    p.initialRole = ScenarioPeer::Role::Leecher;
    p.initialPieceCount = 3;
    p.initialBitfield = std::vector<std::uint8_t>{0xa8}; // {0,2,4}.
    p.position = {65.25, 12.875};
    s.peers.push_back(p);
    project.swarms.push_back(s);
    s.id = 77;
    s.name = "File swarm";
    s.mode = ScenarioSwarm::Mode::File;
    s.filePath = "D:/missing-on-this-computer/example.bin";
    s.trackerPosition = {5, -9};
    s.peers.resize(1);
    s.peers[0].id = std::numeric_limits<quint64>::max();
    s.peers[0].initiallyJoined = false;
    s.peers[0].uploadBytesPerSecond = std::numeric_limits<quint64>::max();
    project.swarms.push_back(s);
    return project;
}
void samePeer(const ScenarioPeer& a, const ScenarioPeer& b) {
    check(a.id == b.id && a.name == b.name && a.initialRole == b.initialRole
        && a.initiallyJoined == b.initiallyJoined && a.initialPieceCount == b.initialPieceCount
        && a.initialBitfield == b.initialBitfield && a.position == b.position
        && a.uploadBytesPerSecond == b.uploadBytesPerSecond && a.downloadBytesPerSecond == b.downloadBytesPerSecond
        && a.uploadDisplayValue == b.uploadDisplayValue && a.uploadDisplayUnit == b.uploadDisplayUnit
        && a.downloadDisplayValue == b.downloadDisplayValue && a.downloadDisplayUnit == b.downloadDisplayUnit,
        "Peer field changed during round trip");
}
void same(const ScenarioProject& a, const ScenarioProject& b) {
    check(a.seed == b.seed && a.settings.bitTorrent.regularRechokeInterval == b.settings.bitTorrent.regularRechokeInterval
        && a.settings.bitTorrent.optimisticUnchokeInterval == b.settings.bitTorrent.optimisticUnchokeInterval
        && a.swarms.size() == b.swarms.size(), "Project fields changed");
    for (std::size_t i = 0; i < a.swarms.size(); ++i) {
        const auto& x = a.swarms[i]; const auto& y = b.swarms[i];
        check(x.id == y.id && x.name == y.name && x.mode == y.mode && x.filePath == y.filePath
            && x.totalSizeBytes == y.totalSizeBytes && x.pieceSizeBytes == y.pieceSizeBytes
            && x.blockSizeBytes == y.blockSizeBytes && x.pieceCount == y.pieceCount
            && x.trackerPosition == y.trackerPosition && x.peers.size() == y.peers.size()
            && x.virtualSizeDisplayValue == y.virtualSizeDisplayValue && x.virtualSizeDisplayUnit == y.virtualSizeDisplayUnit
            && x.pieceSizeDisplayValue == y.pieceSizeDisplayValue && x.pieceSizeDisplayUnit == y.pieceSizeDisplayUnit
            && x.blockSizeDisplayValue == y.blockSizeDisplayValue && x.blockSizeDisplayUnit == y.blockSizeDisplayUnit,
            "Swarm field changed during round trip");
        for (std::size_t j = 0; j < x.peers.size(); ++j) samePeer(x.peers[j], y.peers[j]);
    }
}
void roundTripAndFiles() {
    const auto project = fixture();
    const auto json = ScenarioPersistence::toJson(project);
    check(json.contains("\n") && json.contains("\"format\": \"picoTorrent\"")
        && json.contains("\"initialBitfield\": \"a8\""), "Expected readable versioned JSON and exact hex");
    same(project, ScenarioPersistence::fromJson(json));
    QTemporaryDir directory;
    check(directory.isValid(), "Temporary directory unavailable");
    const auto path = directory.filePath("example.pt");
    ScenarioPersistence::save(path, project);
    same(project, ScenarioPersistence::load(path));
    QFile file(path);
    check(file.open(QIODevice::ReadOnly) && file.readAll() == json, ".pt file differs from JSON");
    file.close();
    auto bad = project;
    bad.settings.bitTorrent = {0, 30};
    bool rejected = false;
    try { ScenarioPersistence::save(path, bad); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Invalid project exported");
    same(project, ScenarioPersistence::load(path)); // Failed export did not truncate good file.
    rejected = false;
    try { ScenarioPersistence::load(directory.filePath("missing.pt")); }
    catch (const std::exception&) { rejected = true; }
    check(rejected, "Missing file accepted");
    ScenarioProject empty;
    same(empty, ScenarioPersistence::fromJson(ScenarioPersistence::toJson(empty)));
    auto inactive = project;
    for (auto& s : inactive.swarms) for (auto& p : s.peers) p.initiallyJoined = false;
    inactive.swarms[0].peers[1].initialBitfield.reset();
    same(inactive, ScenarioPersistence::fromJson(ScenarioPersistence::toJson(inactive)));
}
void malformedIsTransactional() {
    const auto original = fixture();
    const auto root = QJsonDocument::fromJson(ScenarioPersistence::toJson(original)).object();
    auto rejects = [&](const QByteArray& bytes) {
        auto current = original;
        bool rejected = false;
        try { current = ScenarioPersistence::fromJson(bytes); }
        catch (const std::invalid_argument& e) { rejected = !QString::fromUtf8(e.what()).isEmpty(); }
        check(rejected, "Malformed project was accepted or had no error");
        same(original, current);
    };
    for (const auto bytes : {QByteArray("{"), QByteArray("[]"), QByteArray("{}"), QByteArray("null")}) rejects(bytes);
    auto changed = [&](const std::function<void(QJsonObject&)>& edit) {
        auto o = root; edit(o); rejects(QJsonDocument(o).toJson());
    };
    changed([](auto& o) { o["format"] = "other"; });
    changed([](auto& o) { o["version"] = 2; });
    changed([](auto& o) { o["version"] = 1.5; });
    changed([](auto& o) { o.remove("seed"); });
    changed([](auto& o) { o["seed"] = "18446744073709551616"; });
    changed([](auto& o) { o["seed"] = 123; }); // Schema uses lossless decimal strings.
    changed([](auto& o) { o["swarms"] = QJsonObject{}; });
    changed([](auto& o) {
        auto settings = o["settings"].toObject();
        auto bt = settings["bitTorrent"].toObject();
        bt["optimisticUnchokeInterval"] = 1;
        settings["bitTorrent"] = bt; o["settings"] = settings;
    });
    auto swarmEdit = [&](const std::function<void(QJsonObject&)>& edit) {
        changed([&](auto& o) {
            auto swarms = o["swarms"].toArray(); auto s = swarms[1].toObject();
            edit(s); swarms[1] = s; o["swarms"] = swarms;
        });
    };
    swarmEdit([](auto& s) { s["id"] = "9007199254740993"; });
    swarmEdit([](auto& s) { s["pieceCount"] = "7"; });
    swarmEdit([](auto& s) { s["blockSizeBytes"] = "0"; });
    swarmEdit([](auto& s) { s["trackerPosition"] = QJsonObject{{"x", "bad"}, {"y", 0}}; });
    auto peerEdit = [&](const std::function<void(QJsonObject&)>& edit) {
        swarmEdit([&](auto& s) {
            auto peers = s["peers"].toArray(); auto p = peers[0].toObject();
            edit(p); peers[0] = p; s["peers"] = peers;
        });
    };
    peerEdit([](auto& p) { p["id"] = "42"; });
    peerEdit([](auto& p) { p["initialBitfield"] = "zz"; });
    peerEdit([](auto& p) { p["initialBitfield"] = "fc00"; });
    peerEdit([](auto& p) { p["initialBitfield"] = "ff"; });
    peerEdit([](auto& p) { p["initialBitfield"] = "a8"; }); // Inconsistent seeder inventory.
    peerEdit([](auto& p) { p.remove("initialBitfield"); });
    peerEdit([](auto& p) { p["uploadBytesPerSecond"] = "0"; });
    peerEdit([](auto& p) { p["initiallyJoined"] = "true"; });
    peerEdit([](auto& p) { p["initialRole"] = "unknown"; });
    peerEdit([](auto& p) { p["position"] = QJsonObject{{"x", QJsonValue::Null}, {"y", 0}}; });
}
void stoppedSnapshot() {
    auto project = fixture();
    auto runtime = RuntimeSession::create(project.swarms, {-100,-100,200,200}, project.seed, project.settings);
    bool complete = false;
    for (unsigned i = 0; i < 10000 && runtime->simulation().step(); ++i) {
        if (inspectPeer(project.swarms[0], project.swarms[0].peers[1], runtime.get()).complete()) {
            complete = true; break;
        }
    }
    check(complete, "Fixture never completed");
    runtime->snapshotToScenario(project.swarms);
    runtime.reset();
    const auto bytes = ScenarioPersistence::toJson(project);
    check(!bytes.contains("receivedBlocks") && !bytes.contains("currentTime") && !bytes.contains("connections"),
        "Runtime-only fields were exported");
    auto imported = ScenarioPersistence::fromJson(bytes);
    same(project, imported);
    check(imported.swarms[0].peers[1].initialBitfield == std::vector<std::uint8_t>{0xfc},
        "Completed snapshot bitmap was lost");
    auto fresh = RuntimeSession::create(imported.swarms, {-100,-100,200,200}, imported.seed, imported.settings);
    check(fresh->simulation().currentTime() == 0 && fresh->network().links().empty()
        && fresh->network().peer(2).swarmState(1).receivedBlocks.empty(),
        "Import resumed old runtime state");
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        roundTripAndFiles(); malformedIsTransactional(); stoppedSnapshot();
        std::cout << "Scenario persistence tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
