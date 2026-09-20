#include "PeerInspection.hpp"
#include "RuntimeSession.hpp"
#include <bit>

PeerInspection inspectPeer(const ScenarioSwarm& swarm, const ScenarioPeer& peer,
                           const RuntimeSession* runtime)
{
    PeerInspection result;
    result.runtime = runtime != nullptr;
    result.joined = !runtime && peer.initiallyJoined;
    result.pieceCount = swarm.pieceCount;
    result.pieces = peer.initialBitfield;
    result.ownedPieces = peer.initialRole == ScenarioPeer::Role::Seeder ? swarm.pieceCount : peer.initialPieceCount;
    result.uploadBytesPerSecond = static_cast<double>(peer.uploadBytesPerSecond);
    result.downloadBytesPerSecond = static_cast<double>(peer.downloadBytesPerSecond);
    if (runtime) {
        const auto binding = runtime->peerBindings().find(peer.id);
        if (binding != runtime->peerBindings().end() && binding->second.scenarioSwarmId == swarm.id) {
            const auto& ids = binding->second;
            const auto& engine = runtime->network().peer(ids.peerId);
            result.enginePeerId = engine.id();
            result.pieceCount = runtime->network().swarm(ids.swarmId).pieceCount();
            result.uploadBytesPerSecond = engine.uploadCapacity() / 8;
            result.downloadBytesPerSecond = engine.downloadCapacity() / 8;
            result.joined = engine.isActiveInSwarm(ids.swarmId);
            result.pieces = ids.initialBitfield; // Never-joined membership has no engine bitfield.
            if (engine.hasSwarm(ids.swarmId)) {
                const auto& state = engine.swarmState(ids.swarmId);
                result.pieces = state.localBitfield;
                result.connections = state.connections.size();
                for (const auto& [remote, connection] : state.connections) {
                    result.established += connection.handshakeComplete();
                    result.chokingUs += connection.remoteIsChokingUs;
                    result.weChoke += connection.weAreChokingRemote;
                    result.interestedInUs += connection.remoteInterestedInUs;
                    result.weInterested += connection.weAreInterestedInRemote;
                    result.outstanding += connection.outgoingRequests.size();
                    result.reserved += connection.scheduledRequests.size();
                }
            }
        }
    }
    if (result.pieces) {
        result.ownedPieces = 0;
        for (const auto byte : *result.pieces) result.ownedPieces += std::popcount(byte);
    }
    return result;
}
