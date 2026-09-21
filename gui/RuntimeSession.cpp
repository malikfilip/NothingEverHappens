#include "RuntimeSession.hpp"
#include "ScenarioPieces.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
constexpr auto maxEngineValue = std::numeric_limits<std::uint32_t>::max();

bool participates(const ScenarioSwarm& swarm)
{
    return std::any_of(swarm.peers.begin(), swarm.peers.end(),
        [](const ScenarioPeer& peer) { return peer.initiallyJoined; });
}

// NOT a torrent hash or file digest. A versioned simulation namespace followed by
// the full stable scenario ID (big endian) gives distinct synthetic identities.
std::array<std::uint8_t, 20> simulationIdentity(quint64 id, bool peer)
{
    std::array<std::uint8_t, 20> result{'P','I','C','O','-','S','I','M','-',
        static_cast<std::uint8_t>(peer ? 'P' : 'S'), static_cast<std::uint8_t>(peer ? 'R' : 'W'), 1};
    for (unsigned i = 0; i < 8; ++i) result[12 + i] = static_cast<std::uint8_t>(id >> (56 - 8 * i));
    return result;
}


}

PlayPreflight RuntimeSession::preflight(const std::vector<ScenarioSwarm>& scenario, bool forPlayback)
{
    PlayPreflight result;
    std::set<quint64> swarmIds, peerIds;
    std::uint64_t peerCount = 0;
    for (const auto& swarm : scenario) {
        const auto prefix = QStringLiteral("Swarm '%1': ").arg(swarm.name);
        if (!swarmIds.insert(swarm.id).second || swarm.id == 0)
            result.errors << prefix + QStringLiteral("scenario ID must be unique and nonzero.");
        const auto joined = std::count_if(swarm.peers.begin(), swarm.peers.end(),
            [](const ScenarioPeer& peer) { return peer.initiallyJoined; });
        if (!joined) {
            result.skippedSwarms << swarm.name;
            if (forPlayback) continue;
        }
        result.participatingSwarms.push_back(swarm.id);
        if (joined == 1) result.singlePeerSwarms << swarm.name;
        if (std::none_of(swarm.peers.begin(), swarm.peers.end(), [](const ScenarioPeer& peer) {
            return peer.initiallyJoined && peer.initialRole == ScenarioPeer::Role::Leecher;
        })) result.noLeecherSwarms << swarm.name;

        const bool validSizes = swarm.totalSizeBytes > 0 && swarm.pieceSizeBytes > 0
            && swarm.pieceSizeBytes <= maxEngineValue;
        quint64 derivedCount = 0;
        if (validSizes) derivedCount = swarm.totalSizeBytes / swarm.pieceSizeBytes
            + (swarm.totalSizeBytes % swarm.pieceSizeBytes != 0);
        if (!validSizes || derivedCount > maxEngineValue || derivedCount != swarm.pieceCount)
            result.errors << prefix + QStringLiteral("invalid sizes or piece count; engine piece length and count must fit uint32.");
        // Preserve old scenarios with sub-16-KiB pieces and the implicit default.
        if (!swarm.blockSizeBytes || swarm.blockSizeBytes > maxEngineValue
            || (swarm.blockSizeBytes > swarm.pieceSizeBytes
                && swarm.blockSizeBytes != simulator::Swarm::defaultBlockSize))
            result.errors << prefix + QStringLiteral("block size must be positive and no larger than piece size.");
        if (swarm.name.trimmed().isEmpty()) result.errors << prefix + QStringLiteral("name is empty.");
        peerCount += swarm.peers.size();
        for (const auto& peer : swarm.peers) {
            const auto peerPrefix = prefix + QStringLiteral("peer '%1': ").arg(peer.name);
            if (!peerIds.insert(peer.id).second || peer.id == 0)
                result.errors << peerPrefix + QStringLiteral("scenario peer ID must be globally unique and nonzero.");
            if (peer.name.trimmed().isEmpty() || !peer.uploadBytesPerSecond || !peer.downloadBytesPerSecond)
                result.errors << peerPrefix + QStringLiteral("name and positive upload/download capacities are required.");
            if (!std::isfinite(peer.position.x()) || !std::isfinite(peer.position.y()))
                result.errors << peerPrefix + QStringLiteral("position must be finite.");
            const auto expected = peer.initialRole == ScenarioPeer::Role::Seeder ? swarm.pieceCount : peer.initialPieceCount;
            if (peer.initialPieceCount > swarm.pieceCount)
                result.errors << peerPrefix + QStringLiteral("initial piece count exceeds swarm size.");
            if (peer.initialBitfield && derivedCount > 0 && derivedCount <= maxEngineValue) {
                const auto& bits = *peer.initialBitfield;
                bool valid = bits.size() == (derivedCount + 7) / 8;
                if (valid && derivedCount % 8)
                    valid = (bits.back() & ((1u << (8 - derivedCount % 8)) - 1u)) == 0;
                quint64 owned = 0;
                if (valid) for (const auto byte : bits) owned += std::popcount(byte);
                if (!valid || owned != expected)
                    result.errors << peerPrefix + QStringLiteral("concrete piece ownership does not match the configured count/role.");
            }
        }
    }
    if (forPlayback && result.participatingSwarms.empty())
        result.errors << QStringLiteral("At least one swarm must contain an initially joined peer.");
    if (result.participatingSwarms.size() > maxEngineValue || peerCount > maxEngineValue)
        result.errors << QStringLiteral("Too many swarms or peers for engine IDs.");
    return result;
}

std::unique_ptr<RuntimeSession> RuntimeSession::create(std::vector<ScenarioSwarm>& scenario,
                                                     const QRectF& canvas, std::uint64_t seed, ScenarioSettings settings)
{
    if (const auto* error = settings.bitTorrent.validationError()) throw std::invalid_argument(error);
    const auto validation = preflight(scenario);
    if (!validation.errors.isEmpty()) throw std::invalid_argument(validation.errors.join('\n').toStdString());
    if (!std::isfinite(canvas.x()) || !std::isfinite(canvas.y())
        || !std::isfinite(canvas.width()) || !std::isfinite(canvas.height())
        || canvas.width() <= 0 || canvas.height() <= 0)
        throw std::invalid_argument("Canvas dimensions must be finite and positive.");
    auto prepared = scenario;
    auto session = std::unique_ptr<RuntimeSession>(new RuntimeSession(prepared, canvas, seed, settings));
    scenario.swap(prepared);
    return session;
}

RuntimeSession::RuntimeSession(std::vector<ScenarioSwarm>& scenario, const QRectF& canvas, std::uint64_t seed, ScenarioSettings settings)
    : settings_(settings), simulation_(false, seed)
{
    std::vector<simulator::Swarm> swarms;
    std::vector<simulator::Peer> peers;
    std::vector<quint64> initiallyJoined;
    std::map<simulator::PeerId, QPointF> frozenPositions;
    for (auto& swarm : scenario) {
        if (!participates(swarm)) continue;
        const auto swarmId = static_cast<simulator::SwarmId>(swarms.size() + 1);
        swarmIds_.emplace(swarm.id, swarmId);
        if (swarm.blockSizeBytes == simulator::Swarm::defaultBlockSize)
            swarms.emplace_back(swarmId, simulationIdentity(swarm.id, false), swarm.totalSizeBytes,
                static_cast<std::uint32_t>(swarm.pieceSizeBytes));
        else
            swarms.emplace_back(swarmId, simulationIdentity(swarm.id, false), swarm.totalSizeBytes,
                static_cast<std::uint32_t>(swarm.pieceSizeBytes), static_cast<std::uint32_t>(swarm.blockSizeBytes));
        for (auto& peer : swarm.peers) {
            const auto peerId = static_cast<simulator::PeerId>(peers.size() + 1);
            ensureScenarioPieces(swarm, peer, simulation_.seed());
            // Engine rates are double bits/s; scenario retains exact integer bytes/s.
            peers.emplace_back(peerId, double(peer.uploadBytesPerSecond) * 8.0,
                double(peer.downloadBytesPerSecond) * 8.0, simulationIdentity(peer.id, true));
            const QPointF normalized(std::clamp((peer.position.x() - canvas.left()) / canvas.width(), 0.0, 1.0),
                std::clamp((peer.position.y() - canvas.top()) / canvas.height(), 0.0, 1.0));
            frozenPositions.emplace(peerId, normalized);
            peerBindings_.emplace(peer.id, PeerBinding{swarm.id, swarmId, peerId, normalized, *peer.initialBitfield});
            if (peer.initiallyJoined) initiallyJoined.push_back(peer.id);
        }
    }
    network_ = std::make_unique<simulator::Network>(simulation_, std::move(peers),
        std::vector<simulator::Link>{}, std::move(swarms), 50, 1800, settings_.bitTorrent);
    // Immutable normalized geometry: 1..50 ms over the canvas diagonal.
    // Default link capacity is 100 MiB/s, converted to engine bits/s.
    // Tracker position and future GUI resizes cannot affect this provider.
    network_->setLinkConfigProvider([positions = std::move(frozenPositions)](simulator::PeerId a, simulator::PeerId b) {
        const auto delta = positions.at(a) - positions.at(b);
        const auto distance = std::clamp(std::hypot(delta.x(), delta.y()) / std::sqrt(2.0), 0.0, 1.0);
        return simulator::LinkConfig{100.0 * 1024 * 1024 * 8, 0.001 + distance * 0.049};
    });
    for (const auto scenarioPeerId : initiallyJoined) {
        const auto& binding = peerBindings_.at(scenarioPeerId);
        simulator::JoinOptions options;
        options.initialBitfield = binding.initialBitfield;
        network_->joinSwarm(binding.swarmId, binding.peerId, std::move(options));
    }
    // No step()/run(): STARTED events intentionally remain queued at t=0.
}

void RuntimeSession::snapshotToScenario(std::vector<ScenarioSwarm>& scenario) const
{
    // Allocate/copy before publishing so a failed snapshot leaves the editor intact.
    auto snapshot = scenario;
    for (auto& swarm : snapshot) for (auto& peer : swarm.peers) {
        const auto found = peerBindings_.find(peer.id);
        if (found == peerBindings_.end() || found->second.scenarioSwarmId != swarm.id)
            continue; // Skipped swarms never entered this session.
        const auto& binding = found->second;
        const auto& enginePeer = network_->peer(binding.peerId);
        peer.initiallyJoined = enginePeer.isActiveInSwarm(binding.swarmId);
        // Saved inactive memberships retain ownership too. Never-joined peers use
        // their frozen initial inventory. receivedBlocks deliberately is NOT copied.
        peer.initialBitfield = enginePeer.hasSwarm(binding.swarmId)
            ? enginePeer.swarmState(binding.swarmId).localBitfield : binding.initialBitfield;
        peer.initialPieceCount = 0;
        for (const auto byte : *peer.initialBitfield)
            peer.initialPieceCount += std::popcount(byte);
        // These existing editor fields are derived from the exact bitmap, never
        // used to reconstruct it or to persist a separate runtime role.
        peer.initialRole = peer.initialPieceCount == swarm.pieceCount
            ? ScenarioPeer::Role::Seeder : ScenarioPeer::Role::Leecher;
    }
    scenario.swap(snapshot);
}
