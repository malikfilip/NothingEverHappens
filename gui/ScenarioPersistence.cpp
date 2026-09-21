#include "ScenarioPersistence.hpp"
#include "RuntimeSession.hpp"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <cmath>
#include <stdexcept>

namespace {
[[noreturn]] void invalid(const QString& message) {
    throw std::invalid_argument(message.toStdString());
}
QJsonObject object(const QJsonValue& value, const QString& field) {
    if (!value.isObject()) invalid(field + ": expected an object.");
    return value.toObject();
}
QJsonArray array(const QJsonValue& value, const QString& field) {
    if (!value.isArray()) invalid(field + ": expected an array.");
    return value.toArray();
}
QString string(const QJsonObject& o, const QString& key) {
    if (!o.value(key).isString()) invalid(key + ": expected a string.");
    return o.value(key).toString();
}
quint64 integer(const QJsonObject& o, const QString& key) {
    const auto value = string(o, key);
    if (value.isEmpty()) invalid(key + ": expected an unsigned decimal string.");
    for (const auto c : value)
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            invalid(key + ": expected an unsigned decimal string.");
    bool ok = false;
    const auto result = value.toULongLong(&ok);
    if (!ok) invalid(key + ": value exceeds uint64.");
    return result;
}
double number(const QJsonObject& o, const QString& key) {
    const auto value = o.value(key);
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        invalid(key + ": expected a finite number.");
    return value.toDouble();
}
bool boolean(const QJsonObject& o, const QString& key) {
    if (!o.value(key).isBool()) invalid(key + ": expected a boolean.");
    return o.value(key).toBool();
}
QJsonObject point(QPointF p) { return {{"x", p.x()}, {"y", p.y()}}; }
QPointF point(const QJsonObject& o, const QString& key) {
    const auto p = object(o.value(key), key);
    return {number(p, "x"), number(p, "y")};
}
void validate(const ScenarioProject& project) {
    if (const auto* error = project.settings.bitTorrent.validationError()) invalid(error);
    const auto errors = RuntimeSession::preflight(project.swarms, false).errors;
    if (!errors.isEmpty()) invalid(errors.join('\n'));
    for (const auto& s : project.swarms) {
        if (s.mode != ScenarioSwarm::Mode::File && s.mode != ScenarioSwarm::Mode::Virtual)
            invalid("Invalid swarm mode.");
        if (!std::isfinite(s.trackerPosition.x()) || !std::isfinite(s.trackerPosition.y()))
            invalid("Tracker position must be finite.");
        for (const auto& p : s.peers)
            if (p.initialRole != ScenarioPeer::Role::Seeder && p.initialRole != ScenarioPeer::Role::Leecher)
                invalid("Invalid peer role.");
    }
}
QJsonObject peerJson(const ScenarioPeer& p) {
    QJsonObject o;
#define UINT_FIELD(name) o[#name] = QString::number(p.name)
#define TEXT_FIELD(name) o[#name] = p.name
    UINT_FIELD(id); UINT_FIELD(initialPieceCount);
    UINT_FIELD(uploadBytesPerSecond); UINT_FIELD(downloadBytesPerSecond);
    TEXT_FIELD(name); TEXT_FIELD(uploadDisplayValue); TEXT_FIELD(uploadDisplayUnit);
    TEXT_FIELD(downloadDisplayValue); TEXT_FIELD(downloadDisplayUnit);
#undef UINT_FIELD
#undef TEXT_FIELD
    o["initialRole"] = p.initialRole == ScenarioPeer::Role::Seeder ? "seeder" : "leecher";
    o["initiallyJoined"] = p.initiallyJoined;
    o["position"] = point(p.position);
    if (p.initialBitfield) {
        const auto& bits = *p.initialBitfield;
        o["initialBitfield"] = QString::fromLatin1(QByteArray(
            reinterpret_cast<const char*>(bits.data()), qsizetype(bits.size())).toHex());
    } else o["initialBitfield"] = QJsonValue::Null;
    return o;
}
ScenarioPeer readPeer(const QJsonValue& value) {
    const auto o = object(value, "peer");
    ScenarioPeer p;
#define UINT_FIELD(name) p.name = integer(o, #name)
#define TEXT_FIELD(name) p.name = string(o, #name)
    UINT_FIELD(id); UINT_FIELD(initialPieceCount);
    UINT_FIELD(uploadBytesPerSecond); UINT_FIELD(downloadBytesPerSecond);
    TEXT_FIELD(name); TEXT_FIELD(uploadDisplayValue); TEXT_FIELD(uploadDisplayUnit);
    TEXT_FIELD(downloadDisplayValue); TEXT_FIELD(downloadDisplayUnit);
#undef UINT_FIELD
#undef TEXT_FIELD
    const auto role = string(o, "initialRole");
    if (role != "seeder" && role != "leecher") invalid("initialRole: expected seeder or leecher.");
    p.initialRole = role == "seeder" ? ScenarioPeer::Role::Seeder : ScenarioPeer::Role::Leecher;
    p.initiallyJoined = boolean(o, "initiallyJoined");
    p.position = point(o, "position");
    const auto bits = o.value("initialBitfield");
    if (!bits.isNull()) {
        const auto hex = string(o, "initialBitfield");
        if (hex.size() % 2) invalid("initialBitfield: expected pairs of hexadecimal digits.");
        for (auto c : hex)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                invalid("initialBitfield: invalid hexadecimal digit.");
        const auto bytes = QByteArray::fromHex(hex.toLatin1());
        p.initialBitfield = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }
    return p;
}
QJsonObject swarmJson(const ScenarioSwarm& s) {
    QJsonObject o;
#define UINT_FIELD(name) o[#name] = QString::number(s.name)
#define TEXT_FIELD(name) o[#name] = s.name
    UINT_FIELD(id); UINT_FIELD(totalSizeBytes); UINT_FIELD(pieceSizeBytes);
    UINT_FIELD(pieceCount); UINT_FIELD(blockSizeBytes);
    TEXT_FIELD(name); TEXT_FIELD(filePath);
    TEXT_FIELD(virtualSizeDisplayValue); TEXT_FIELD(virtualSizeDisplayUnit);
    TEXT_FIELD(pieceSizeDisplayValue); TEXT_FIELD(pieceSizeDisplayUnit);
    TEXT_FIELD(blockSizeDisplayValue); TEXT_FIELD(blockSizeDisplayUnit);
#undef UINT_FIELD
#undef TEXT_FIELD
    o["mode"] = s.mode == ScenarioSwarm::Mode::File ? "file" : "virtual";
    o["trackerPosition"] = point(s.trackerPosition);
    QJsonArray peers;
    for (const auto& p : s.peers) peers.append(peerJson(p));
    o["peers"] = peers;
    return o;
}
ScenarioSwarm readSwarm(const QJsonValue& value) {
    const auto o = object(value, "swarm");
    ScenarioSwarm s;
#define UINT_FIELD(name) s.name = integer(o, #name)
#define TEXT_FIELD(name) s.name = string(o, #name)
    UINT_FIELD(id); UINT_FIELD(totalSizeBytes); UINT_FIELD(pieceSizeBytes);
    UINT_FIELD(pieceCount); UINT_FIELD(blockSizeBytes);
    TEXT_FIELD(name); TEXT_FIELD(filePath);
    TEXT_FIELD(virtualSizeDisplayValue); TEXT_FIELD(virtualSizeDisplayUnit);
    TEXT_FIELD(pieceSizeDisplayValue); TEXT_FIELD(pieceSizeDisplayUnit);
    TEXT_FIELD(blockSizeDisplayValue); TEXT_FIELD(blockSizeDisplayUnit);
#undef UINT_FIELD
#undef TEXT_FIELD
    const auto mode = string(o, "mode");
    if (mode != "file" && mode != "virtual") invalid("mode: expected file or virtual.");
    s.mode = mode == "file" ? ScenarioSwarm::Mode::File : ScenarioSwarm::Mode::Virtual;
    s.trackerPosition = point(o, "trackerPosition");
    for (const auto& p : array(o.value("peers"), "peers")) s.peers.push_back(readPeer(p));
    return s;
}
}

QByteArray ScenarioPersistence::toJson(const ScenarioProject& project) {
    validate(project);
    QJsonArray swarms;
    for (const auto& s : project.swarms) swarms.append(swarmJson(s));
    const auto& settings = project.settings.bitTorrent;
    return QJsonDocument(QJsonObject{
        {"format", "picoTorrent"}, {"version", 1}, {"seed", QString::number(project.seed)},
        {"settings", QJsonObject{{"bitTorrent", QJsonObject{
            {"regularRechokeInterval", settings.regularRechokeInterval},
            {"optimisticUnchokeInterval", settings.optimisticUnchokeInterval}}}}},
        {"swarms", swarms}}).toJson(QJsonDocument::Indented);
}

ScenarioProject ScenarioPersistence::fromJson(const QByteArray& json) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError)
        invalid(QString("JSON error at byte %1: %2").arg(error.offset).arg(error.errorString()));
    if (!document.isObject()) invalid("Expected a picoTorrent project object.");
    const auto root = document.object();
    if (string(root, "format") != "picoTorrent") invalid("Unsupported scenario format.");
    if (number(root, "version") != 1) invalid("Unsupported picoTorrent scenario version (expected 1).");
    ScenarioProject result;
    result.seed = integer(root, "seed");
    const auto settings = object(root.value("settings"), "settings");
    const auto bt = object(settings.value("bitTorrent"), "settings.bitTorrent");
    result.settings.bitTorrent = {number(bt, "regularRechokeInterval"), number(bt, "optimisticUnchokeInterval")};
    for (const auto& s : array(root.value("swarms"), "swarms")) result.swarms.push_back(readSwarm(s));
    validate(result);
    return result;
}

void ScenarioPersistence::save(const QString& path, const ScenarioProject& project) {
    const auto json = toJson(project);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
        throw std::runtime_error(("Could not save " + path + ": " + file.errorString()).toStdString());
}
ScenarioProject ScenarioPersistence::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(("Could not open " + path + ": " + file.errorString()).toStdString());
    const auto data = file.readAll();
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error(("Could not read " + path + ": " + file.errorString()).toStdString());
    return fromJson(data);
}
