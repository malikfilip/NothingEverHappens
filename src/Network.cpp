#include "simulator/Network.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <tuple>

#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
#include "simulator/TransmissionStartEvent.hpp"

namespace simulator {

    namespace {
        constexpr std::size_t requestPipelineDepth = 5;
    }

    Network::Network(Simulation& simulation, std::vector<Peer> peers, std::vector<Link> links, std::vector<Swarm> swarms, std::size_t trackerMaximum, double trackerInterval)
        : tracker_(trackerMaximum, simulation.seed(), trackerInterval), simulation_(simulation), peers_(std::move(peers)), links_(std::move(links)), swarms_(std::move(swarms))
    {
        for (std::size_t i = 0; i < peers_.size(); ++i) {
            peer_transport_.try_emplace(peers_[i].id(), PeerTransport{i, {}, {}});
        }
    }

    const Peer& Network::peer(PeerId id) const
    {
        const auto found = peer_transport_.find(id);
        if (found == peer_transport_.end()) {
            throw std::invalid_argument("Unknown peer");
        }
        return peers_[found->second.peerIndex];
    }

    const Swarm& Network::swarm(SwarmId id) const
    {
        const auto found = std::find_if(swarms_.begin(), swarms_.end(),
            [id](const Swarm& swarm) { return swarm.id() == id; });
        if (found == swarms_.end()) {
            throw std::invalid_argument("Unknown swarm");
        }
        return *found;
    }

    void Network::deliver(SwarmId swarmId, PeerId sender, PeerId receiver, const Message& message)
    {
        const auto to = std::find_if(peers_.begin(), peers_.end(),
            [receiver](const Peer& peer) { return peer.id() == receiver; });
        if (to == peers_.end()) {
            throw std::invalid_argument("Unknown receiver");
        }
        const PeerProtocolId* senderProtocolId = nullptr;
        if (message.type() == MessageType::Handshake) {
            senderProtocolId = &peer(sender).protocolId();
        }
        if ((to->hasSwarm(swarmId) && !to->isActiveInSwarm(swarmId))
            || (peer(sender).hasSwarm(swarmId) && !peer(sender).isActiveInSwarm(swarmId)))
            throw std::invalid_argument("Inactive swarm membership");
        const auto& currentSwarm = swarm(swarmId);
        const bool availability = message.type() == MessageType::Bitfield || message.type() == MessageType::Have;
        const bool interestMessage = message.type() == MessageType::Interested
            || message.type() == MessageType::NotInterested;

        bool wasInterested = false;
        if ((availability || interestMessage) && to->hasSwarm(swarmId)) {
            const auto& connections = to->swarmState(swarmId).connections;
            const auto connection = connections.find(sender);
            if (connection != connections.end()) {
                wasInterested = connection->second.weAreInterestedInRemote;

            }
        }
        // A normal request can cross a policy CHOKE in flight. Drop that obsolete
        // service attempt; the next UNCHOKE can reserve it again.
        if (message.type() == MessageType::Request && to->hasSwarm(swarmId)
            && to->swarmState(swarmId).choking.managed) {
            const auto& connections = to->swarmState(swarmId).connections;
            const auto c = connections.find(sender);
            if (c != connections.end() && c->second.handshakeComplete()
                && peer(sender).hasSwarm(swarmId)) {
                auto& remote = peers_[peer_transport_.at(sender).peerIndex].swarm_states_.at(swarmId).connections.at(receiver);
                if (c->second.weAreChokingRemote) {
                    std::erase(remote.outgoingRequests, std::get<RequestPayload>(message.payload()));
                    return;
                }
            }
        }
        std::size_t acceptedBefore = 0;
        if (message.type() == MessageType::Request && to->hasSwarm(swarmId)) {
            const auto& connections = to->swarmState(swarmId).connections;
            if (connections.contains(sender)) acceptedBefore = connections.at(sender).acceptedRequests.size();
        }
        bool pieceWasOwned = false;
        bool retiredPiece = false;
        if (message.type() == MessageType::Piece && to->hasSwarm(swarmId)) {
            const auto& p = std::get<PiecePayload>(message.payload());
            const auto& connections = to->swarmState(swarmId).connections;
            if (const auto c = connections.find(sender); c != connections.end()) {
                const auto& retired = c->second.retiredRequests;
                retiredPiece = std::find(retired.begin(), retired.end(),
                    RequestPayload{p.index, p.begin, p.length}) != retired.end();
            }
        }
        std::uint64_t usefulBytes = 0;
        if (message.type() == MessageType::Piece && to->hasSwarm(swarmId)) {
            const auto& payload = std::get<PiecePayload>(message.payload());
            const auto index = payload.index;
            if (index < currentSwarm.pieceCount()) {
                pieceWasOwned = (to->swarmState(swarmId).localBitfield[index / 8]
                    & (0x80u >> (index % 8))) != 0;
            }
        }
        if (message.type() == MessageType::Piece && !pieceWasOwned && !retiredPiece) {
            const auto& payload = std::get<PiecePayload>(message.payload());
            usefulBytes = payload.length;
            const auto& ranges = to->swarmState(swarmId).receivedBlocks;
            if (const auto found = ranges.find(payload.index); found != ranges.end()) {
                for (const auto& range : found->second) {
                    const auto begin = std::max<std::uint64_t>(range.begin, payload.begin);
                    const auto end = std::min<std::uint64_t>(range.end, std::uint64_t(payload.begin) + payload.length);
                    if (end > begin) usefulBytes -= end - begin;
                }
            }
        }
        to->receiveMessage(currentSwarm, sender, message, senderProtocolId,
            (message.type() == MessageType::Request || message.type() == MessageType::Piece) ? &peer(sender) : nullptr);
        if (message.type() == MessageType::Cancel) {
            const auto& cancel = std::get<CancelPayload>(message.payload());
            const RequestPayload block{cancel.index, cancel.begin, cancel.length};
            const auto& committed = to->swarmState(swarmId).connections.at(sender).committedRequests;
            if (std::find(committed.begin(), committed.end(), block) == committed.end()) {
                auto& connections = peers_[peer_transport_.at(sender).peerIndex].swarm_states_.at(swarmId).connections;
                if (auto c = connections.find(receiver); c != connections.end())
                    std::erase(c->second.retiredRequests, block);
            }
        }
        if (message.type() == MessageType::Request
            && to->swarmState(swarmId).connections.at(sender).acceptedRequests.size() != acceptedBefore) {
            const auto& request = std::get<RequestPayload>(message.payload());
            simulation_.schedule(std::make_unique<SendMessageEvent>(
                simulation_.currentTime(), *this, swarmId, receiver, sender,
                Message(MessageType::Piece, PiecePayload{request.index, request.begin, request.length}), false, true));
        }
        if (message.type() == MessageType::Piece) {
            const auto& piece = std::get<PiecePayload>(message.payload());
            const auto from = std::find_if(peers_.begin(), peers_.end(),
                [sender](const Peer& candidate) { return candidate.id() == sender; });
            const RequestPayload block{piece.index, piece.begin, piece.length};
            auto& provider = from->swarm_states_.at(swarmId).connections.at(receiver);
            std::erase(provider.acceptedRequests, block);
            std::erase(provider.committedRequests, block);
            if (retiredPiece) return; // Expected late wire traffic, never useful payload.
            recordUsefulPiece(sender, receiver, swarmId, usefulBytes);
            std::vector<PeerId> duplicates;
            for (auto& [remote, connection] : to->swarm_states_.at(swarmId).connections) {
                std::erase(connection.scheduledRequests, block);
                if (remote != sender && std::erase(connection.outgoingRequests, block)) {
                    connection.retiredRequests.push_back(block);
                    duplicates.push_back(remote);
                }
            }
            std::sort(duplicates.begin(), duplicates.end());
            for (const auto remote : duplicates) {
                simulation_.schedule(std::make_unique<SendMessageEvent>(
                    simulation_.currentTime(), *this, swarmId, receiver, remote,
                    Message(MessageType::Cancel, CancelPayload{block.index, block.begin, block.length})));
            }
            const auto& state = to->swarmState(swarmId);
            if (!pieceWasOwned && (state.localBitfield[piece.index / 8]
                & (0x80u >> (piece.index % 8))) != 0) {
                // Stable broadcast order; unordered connection storage must not affect tracing.
                std::vector<PeerId> recipients;
                for (const auto& [remote, connection] : state.connections) {
                    if (connection.handshakeComplete()) recipients.push_back(remote);
                }
                std::sort(recipients.begin(), recipients.end());
                for (const auto remote : recipients) {
                    simulation_.schedule(std::make_unique<SendMessageEvent>(
                        simulation_.currentTime(), *this, swarmId, receiver, remote,
                        Message(MessageType::Have, HavePayload{piece.index})));
                    auto& connection = to->swarm_states_.at(swarmId).connections.at(remote);
                    const bool interested = Peer::hasUsefulPieces(currentSwarm, state, connection);
                    if (interested != connection.weAreInterestedInRemote) {
                        connection.weAreInterestedInRemote = interested;
                        simulation_.schedule(std::make_unique<SendMessageEvent>(
                            simulation_.currentTime(), *this, swarmId, receiver, remote,
                            Message(interested ? MessageType::Interested : MessageType::NotInterested)));
                    }
                }
            }
        }
        if (interestMessage) {
            enforceInterestedLimit(receiver, swarmId);
            if (message.type() == MessageType::Interested
                || !to->swarmState(swarmId).connections.at(sender).weAreChokingRemote) {
                scheduleRechoke(receiver, swarmId);
            }
        }
        if (availability && to->swarmState(swarmId).choking.managed && hasUsefulExchange(receiver, swarmId)) scheduleRechoke(receiver, swarmId);
        if (availability) {
            const bool interested = to->swarmState(swarmId).connections.at(sender).weAreInterestedInRemote;
            if (interested != wasInterested) {
                simulation_.schedule(std::make_unique<SendMessageEvent>(
                    simulation_.currentTime(), *this, swarmId, receiver, sender,
                    Message(interested ? MessageType::Interested : MessageType::NotInterested)));
            }
        }
        if (message.type() == MessageType::Handshake
            && !to->swarmState(swarmId).connections.at(sender).handshakeSent) {
            const auto& link = links_[linkIndex(receiver, sender)];
            const auto& direction = receiver == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
            const bool queued = std::any_of(direction.pending.begin(), direction.pending.end(),
                [swarmId](const QueuedTransmission& transmission) {
                    return transmission.swarmId == swarmId
                        && transmission.message.type() == MessageType::Handshake;
                });
            if (!queued) {
                SendMessageEvent response(simulation_.currentTime(), *this, swarmId, receiver, sender,
                    Message(MessageType::Handshake,
                        HandshakePayload{currentSwarm.infoHash(), to->protocolId()}));
                simulation_.executeNow(response);
            }
        }
        if (message.type() == MessageType::Handshake) {
            sendInitialBitfield(*to, swarmId, sender);
            const auto discovery = discovery_.find({receiver, swarmId});
            if (discovery != discovery_.end() && discovery->second.pending.contains(sender)
                && to->swarmState(swarmId).connections.at(sender).handshakeComplete()
                && peer(sender).swarmState(swarmId).connections.at(receiver).handshakeComplete()) {
                for (const auto& endpoints : {std::pair{sender, receiver}, std::pair{receiver, sender}}) {
                    const auto found = discovery_.find({endpoints.first, swarmId});
                    if (found != discovery_.end()) found->second.pending.erase(endpoints.second);
                }
            }
        }
        if (availability || message.type() == MessageType::Unchoke
            || message.type() == MessageType::Piece || message.type() == MessageType::Handshake) {
            tryScheduleRequests(*to, swarmId, sender);
        }
    }

    std::optional<RequestPayload> Network::nextRequestBlock(const Swarm& swarm,
        const PeerSwarmState& state, std::uint32_t piece, std::uint32_t begin,
        const std::vector<RequestPayload>* retiredAtTarget)
    {
        const auto size = swarm.pieceSize(piece);
        std::vector<BlockRange> occupied;
        if (const auto received = state.receivedBlocks.find(piece); received != state.receivedBlocks.end()) {
            occupied = received->second;
        }
        // Reservations and in-flight blocks on every remote exclude overlapping requests.
        for (const auto& [remote, connection] : state.connections) {
            for (const auto* requests : {&connection.outgoingRequests, &connection.scheduledRequests}) {
                for (const auto& request : *requests) {
                    if (request.index == piece) occupied.push_back({request.begin, request.begin + request.length});
                }
            }
        }
        // Avoid reusing a canceled response identity on this target only. These
        // ranges are NOT reservations for other providers or endgame eligibility.
        if (retiredAtTarget) for (const auto& request : *retiredAtTarget)
            if (request.index == piece) occupied.push_back({request.begin, request.begin + request.length});
        std::sort(occupied.begin(), occupied.end(), [](const auto& a, const auto& b) {
            return a.begin < b.begin;
        });
        for (const auto& range : occupied) {
            if (range.end <= begin) continue;
            if (range.begin > begin) {
                return RequestPayload{piece, begin, std::min(swarm.blockSize(), range.begin - begin)};
            }
            begin = range.end;
        }
        if (begin >= size) return std::nullopt;
        return RequestPayload{piece, begin, std::min(swarm.blockSize(), size - begin)};
    }

    std::optional<std::uint32_t> Network::selectPiece(const Swarm& swarm,
        const PeerSwarmState& state, const PeerConnectionState& connection,
        const PeerSwarmState& remoteState)
    {
        std::optional<std::uint32_t> selected;
        std::size_t lowestAvailability = 0;
        for (std::uint32_t piece = 0; piece < swarm.pieceCount(); ++piece) {
            const auto mask = 0x80u >> (piece % 8);
            if ((state.localBitfield[piece / 8] & mask) != 0
                || (connection.remoteBitfield[piece / 8] & mask) == 0
                || (remoteState.localBitfield[piece / 8] & mask) == 0
                || !nextRequestBlock(swarm, state, piece, 0, &connection.retiredRequests)) continue;

            // Rarity uses only this requester's knowledge, including choked remotes.
            // The target's actual inventory above remains an eligibility safeguard.
            std::size_t availability = 0;
            for (const auto& [remote, known] : state.connections) {
                if (known.handshakeComplete() && (known.remoteBitfield[piece / 8] & mask) != 0) ++availability;
            }
            // Ascending traversal and strict comparison break ties by piece index.
            if (!selected || availability < lowestAvailability) {
                selected = piece;
                lowestAvailability = availability;
            }
        }
        return selected;
    }

    void Network::tryScheduleRequests(Peer& requester, SwarmId swarmId, PeerId remotePeerId)
    {
        if (!requester.isActiveInSwarm(swarmId) || !peer(remotePeerId).isActiveInSwarm(swarmId)) return;
        const auto& currentSwarm = swarm(swarmId);
        if (currentSwarm.pieceLength() == 0 || !requester.hasSwarm(swarmId)) return;
        auto& state = requester.swarm_states_.at(swarmId);
        auto fill = [&](PeerId target) {
            if (!peer(target).isActiveInSwarm(swarmId)) return;
            const auto found = state.connections.find(target);
            if (found == state.connections.end()) return;
            auto& connection = found->second;
            if (!connection.handshakeComplete() || !connection.weAreInterestedInRemote
                || connection.remoteIsChokingUs) return;
            const auto& remoteState = peer(target).swarmState(swarmId);
            while (connection.outgoingRequests.size() + connection.scheduledRequests.size() < requestPipelineDepth) {
                const auto piece = selectPiece(currentSwarm, state, connection, remoteState);
                auto block = piece ? nextRequestBlock(currentSwarm, state, *piece, 0, &connection.retiredRequests)
                    : endgameBlock(currentSwarm, state, target);
                if (!block) break;
                if ((remoteState.localBitfield[block->index / 8] & (0x80u >> (block->index % 8))) == 0) break;
                connection.scheduledRequests.push_back(*block);
                simulation_.schedule(std::make_unique<SendMessageEvent>(
                    simulation_.currentTime(), *this, swarmId, requester.id(), target,
                    Message(MessageType::Request, *block), true));
            }
        };
        fill(remotePeerId);
        // Revisit previously idle neighbors when the last normal gap is reserved.
        if (endgameReady(currentSwarm, state)) {
            std::vector<PeerId> remotes;
            for (const auto& [remote, connection] : state.connections) remotes.push_back(remote);
            std::sort(remotes.begin(), remotes.end());
            for (const auto remote : remotes) if (remote != remotePeerId) fill(remote);
        }
    }

    bool Network::endgameReady(const Swarm& swarm, const PeerSwarmState& state)
    {
        bool incomplete = false;
        for (std::uint32_t piece = 0; piece < swarm.pieceCount(); ++piece) {
            if ((state.localBitfield[piece / 8] & (0x80u >> (piece % 8))) != 0) continue;
            incomplete = true;
            if (nextRequestBlock(swarm, state, piece)) return false;
        }
        return incomplete;
    }

    std::optional<RequestPayload> Network::endgameBlock(const Swarm& swarm,
        const PeerSwarmState& state, PeerId remote)
    {
        if (!endgameReady(swarm, state)) return std::nullopt;
        const auto& target = state.connections.at(remote);
        std::optional<RequestPayload> selected;
        for (const auto& [source, connection] : state.connections) {
            if (source == remote) continue;
            for (const auto* requests : {&connection.scheduledRequests, &connection.outgoingRequests}) {
                for (const auto& block : *requests) {
                    const auto mask = 0x80u >> (block.index % 8);
                    if ((state.localBitfield[block.index / 8] & mask)
                        || !(target.remoteBitfield[block.index / 8] & mask)) continue;
                    bool unavailable = false;
                    for (const auto* own : {&target.scheduledRequests, &target.outgoingRequests, &target.retiredRequests})
                        unavailable |= std::find(own->begin(), own->end(), block) != own->end();
                    if (const auto ranges = state.receivedBlocks.find(block.index); ranges != state.receivedBlocks.end())
                        for (const auto& range : ranges->second)
                            unavailable |= range.begin < block.begin + block.length && block.begin < range.end;
                    if (unavailable) continue;
                    if (!selected || std::tie(block.index, block.begin, block.length)
                        < std::tie(selected->index, selected->begin, selected->length)) selected = block;
                }
            }
        }
        return selected;
    }

    void Network::sendScheduledPiece(SwarmId swarmId, PeerId sender, PeerId receiver, Message message, LifecycleContext context)
    {
        if (messageStale(swarmId, sender, receiver, context)) return;
        auto& from = peers_[peer_transport_.at(sender).peerIndex];
        auto& state = from.swarm_states_.at(swarmId);
        auto& connection = state.connections.at(receiver);
        const auto& piece = std::get<PiecePayload>(message.payload());
        const RequestPayload block{piece.index, piece.begin, piece.length};
        if (std::find(connection.acceptedRequests.begin(), connection.acceptedRequests.end(), block)
            == connection.acceptedRequests.end()) return; // Canceled before transport handoff.
        if (state.choking.managed && connection.weAreChokingRemote) {
            std::erase(connection.acceptedRequests, block);
            std::erase(peers_[peer_transport_.at(receiver).peerIndex].swarm_states_.at(swarmId)
                .connections.at(sender).outgoingRequests, block);
            std::erase(peers_[peer_transport_.at(receiver).peerIndex].swarm_states_.at(swarmId)
                .connections.at(sender).retiredRequests, block);
            return;
        }
        send(swarmId, sender, receiver, std::move(message), context);
    }

    void Network::sendScheduledRequest(SwarmId swarmId, PeerId sender, PeerId receiver, Message message, LifecycleContext context)
    {
        if (messageStale(swarmId, sender, receiver, context)) return;
        const auto from = std::find_if(peers_.begin(), peers_.end(),
            [sender](const Peer& candidate) { return candidate.id() == sender; });
        const auto& block = std::get<RequestPayload>(message.payload());
        auto& state = from->swarm_states_.at(swarmId);
        auto& connection = state.connections.at(receiver);
        if (std::erase(connection.scheduledRequests, block) == 0) return;
        // Notifications at the same timestamp may have changed eligibility or coverage.
        try {
            from->validateRequestTo(swarm(swarmId), peer(receiver), block);
        } catch (const std::invalid_argument&) {
            tryScheduleRequests(*from, swarmId, receiver);
            return;
        }
        if ((connection.remoteBitfield[block.index / 8] & (0x80u >> (block.index % 8))) == 0
            || connection.outgoingRequests.size() >= requestPipelineDepth) {
            tryScheduleRequests(*from, swarmId, receiver);
            return;
        }
        if (nextRequestBlock(swarm(swarmId), state, block.index, block.begin, &connection.retiredRequests) != block
            && endgameBlock(swarm(swarmId), state, receiver) != block) {
            tryScheduleRequests(*from, swarmId, receiver);
            return;
        }
        send(swarmId, sender, receiver, std::move(message), context);
    }

    void Network::sendInitialBitfield(Peer& sender, SwarmId swarmId, PeerId receiver)
    {
        auto& state = sender.swarm_states_.at(swarmId);
        auto& connection = state.connections.at(receiver);
        if (!connection.handshakeComplete() || connection.bitfieldSent) return;

        simulation_.schedule(std::make_unique<SendMessageEvent>(
            simulation_.currentTime(), *this, swarmId, sender.id(), receiver,
            Message(MessageType::Bitfield, BitfieldPayload{state.localBitfield})));
        connection.bitfieldSent = true;
    }

    const Link* Network::link(PeerId endpointA, PeerId endpointB) const
    {
        const auto found = std::find_if(links_.begin(), links_.end(),
            [endpointA, endpointB](const Link& candidate) {
                return (candidate.endpointA() == endpointA && candidate.endpointB() == endpointB)
                    || (candidate.endpointA() == endpointB && candidate.endpointB() == endpointA);
            });
        return found == links_.end() ? nullptr : &*found;
    }

    std::size_t Network::linkIndex(PeerId sender, PeerId receiver) const
    {
        const auto* found = link(sender, receiver);
        if (!found) {
            throw std::invalid_argument("No link connects sender and receiver");
        }
        return static_cast<std::size_t>(found - links_.data());
    }

    Network::TransmissionState Network::transmissionState(PeerId sender, PeerId receiver) const
    {
        const auto& link = links_[linkIndex(sender, receiver)];
        const auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        return {direction.active, direction.pending.size()};
    }

    void Network::send(SwarmId swarmId, PeerId sender, PeerId receiver, Message message, std::optional<LifecycleContext> context)
    {
        const auto lifecycle = context ? *context : lifecycleContext(swarmId, sender, receiver);
        if (context && messageStale(swarmId, sender, receiver, lifecycle)) return;
        const auto& from = peer(sender);
        const auto& to = peer(receiver);
        if ((from.hasSwarm(swarmId) && !from.isActiveInSwarm(swarmId))
            || (to.hasSwarm(swarmId) && !to.isActiveInSwarm(swarmId)))
            throw std::invalid_argument("Inactive swarm membership");
        const auto index = linkIndex(sender, receiver);
        if (message.type() == MessageType::Handshake) {
            swarm(swarmId);
            if (!from.hasSwarm(swarmId)) {
                throw std::invalid_argument("Sending peer has not joined this swarm");
            }
        }
        if (message.type() == MessageType::Request) {
            const auto& request = std::get<RequestPayload>(message.payload());
            from.validateRequestTo(swarm(swarmId), to, request);
            const auto& connection = from.swarmState(swarmId).connections.at(receiver);
            for (const auto* pending : {&connection.outgoingRequests, &connection.retiredRequests})
                if (std::find(pending->begin(), pending->end(), request) != pending->end()) return;
        }
        if (message.type() == MessageType::Cancel) {
            if (!from.isActiveInSwarm(swarmId) || !to.isActiveInSwarm(swarmId))
                throw std::invalid_argument("CANCEL peers must share the swarm");
            const auto& connections = from.swarmState(swarmId).connections;
            const auto connection = connections.find(receiver);
            if (connection == connections.end() || !connection->second.handshakeComplete())
                throw std::invalid_argument("CANCEL requires a completed handshake");
            const auto& cancel = std::get<CancelPayload>(message.payload());
            const RequestPayload block{cancel.index, cancel.begin, cancel.length};
            Peer::validateBlock(swarm(swarmId), block);
            auto& local = peers_[peer_transport_.at(sender).peerIndex].swarm_states_.at(swarmId).connections.at(receiver);
            // Only this remote's exact outstanding request is canceled. Unmatched
            // control traffic must not create permission for unsolicited PIECEs.
            if (std::erase(local.outgoingRequests, block)) {
                if (std::find(local.retiredRequests.begin(), local.retiredRequests.end(), block)
                    == local.retiredRequests.end()) local.retiredRequests.push_back(block);
                std::erase(local.scheduledRequests, block);
            }
        }
        if (message.type() == MessageType::Piece) {
            from.validatePieceTo(swarm(swarmId), to, std::get<PiecePayload>(message.payload()));
        }
        const auto request = message.type() == MessageType::Request
            ? std::optional<RequestPayload>(std::get<RequestPayload>(message.payload())) : std::nullopt;
        auto& link = links_[index];
        auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        if (message.type() == MessageType::Piece) {
            const auto& piece = std::get<PiecePayload>(message.payload());
            auto& committed = peers_[peer_transport_.at(sender).peerIndex].swarm_states_.at(swarmId)
                .connections.at(receiver).committedRequests;
            const RequestPayload block{piece.index, piece.begin, piece.length};
            if (std::find(committed.begin(), committed.end(), block) == committed.end()) committed.push_back(block);
        }
        direction.pending.push_back({swarmId, sender, receiver, std::move(message), lifecycle});
        if (!direction.active) {
            startTransmission(index, sender);
        }
        if (request) {
            const auto mutableFrom = std::find_if(peers_.begin(), peers_.end(),
                [sender](const Peer& candidate) { return candidate.id() == sender; });
            mutableFrom->swarm_states_.at(swarmId).connections.at(receiver).outgoingRequests.push_back(*request);
        }
    }

    void Network::startTransmission(std::size_t index, PeerId sender)
    {
        auto& link = links_[index];
        auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        if (direction.active) return;
        while (!direction.pending.empty()) {
            const auto& queued = direction.pending.front();
            if (!messageStale(queued.swarmId, queued.sender, queued.receiver, queued.lifecycle)) break;
            direction.pending.pop_front();
        }
        if (direction.pending.empty()) return;

        // Only the front message is considered. No timing is assigned while queued.
        auto transmission = std::move(direction.pending.front());
        direction.pending.pop_front();
        TransmissionStartEvent event(simulation_.currentTime(), *this, transmission.swarmId,
            transmission.sender, transmission.receiver, std::move(transmission.message), transmission.lifecycle);
        simulation_.executeNow(event);
    }

    void Network::beginTransmission(QueuedTransmission transmission)
    {
        if (messageStale(transmission.swarmId, transmission.sender, transmission.receiver, transmission.lifecycle)) return;
        const auto sender = transmission.sender;
        const auto index = linkIndex(sender, transmission.receiver);
        auto& link = links_[index];
        auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        if (direction.active) {
            throw std::logic_error("Cannot start a transmission on a busy direction");
        }
        const auto& to = peer(transmission.receiver);
        auto* from = &peers_[peer_transport_.at(sender).peerIndex];
        if (transmission.message.type() == MessageType::Piece) {
            from->validatePieceTo(swarm(transmission.swarmId), to,
                std::get<PiecePayload>(transmission.message.payload()));
        }
        for (double bandwidth : {from->uploadCapacity(), to.downloadCapacity(), link.bandwidth()}) {
            if (!std::isfinite(bandwidth) || bandwidth <= 0.0) {
                throw std::invalid_argument("Transfer bandwidth must be finite and positive");
            }
        }
        if (!std::isfinite(link.latency()) || link.latency() < 0.0) {
            throw std::invalid_argument("Link latency must be finite and nonnegative");
        }
        const double effectiveBandwidth = std::min({
            from->uploadCapacity() / (peer_transport_.at(sender).outgoing.size() + 1),
            to.downloadCapacity() / (peer_transport_.at(transmission.receiver).incoming.size() + 1),
            link.bandwidth()});
        const double bits = static_cast<double>(transmission.message.wireSize()) * 8.0;
        const double transmissionTime = bits / effectiveBandwidth;
        const double completionTime = simulation_.currentTime() + transmissionTime;
        const double arrivalTime = completionTime + link.latency();
        if (!std::isfinite(completionTime) || !std::isfinite(arrivalTime)) {
            throw std::invalid_argument("Transmission completion and arrival times must be finite");
        }
        const bool isHandshake = transmission.message.type() == MessageType::Handshake;
        if (isHandshake) {
            from->markHandshakeSent(swarm(transmission.swarmId), transmission.receiver);
        }
        auto affected = affectedTransmissions(sender, transmission.receiver);
        accountProgress(affected);
        const auto id = next_transmission_id_++;
        const auto active = active_transmissions_.emplace(id, ActiveTransmission{
            id, transmission.swarmId, sender, transmission.receiver,
            std::move(transmission.message), index, bits,
            effectiveBandwidth, simulation_.currentTime(), 0, transmission.lifecycle}).first;
        peer_transport_.at(sender).outgoing.insert(id);
        peer_transport_.at(transmission.receiver).incoming.insert(id);
        recomputeRates(affected);
        direction.active = true;
        simulation_.schedule(std::make_unique<TransmissionCompleteEvent>(
            completionTime, *this, active->second));
        if (isHandshake) {
            sendInitialBitfield(*from, transmission.swarmId, transmission.receiver);
        }
    }

    std::set<TransmissionId> Network::affectedTransmissions(PeerId sender, PeerId receiver) const
    {
        auto ids = peer_transport_.at(sender).outgoing;
        const auto& incoming = peer_transport_.at(receiver).incoming;
        ids.insert(incoming.begin(), incoming.end());
        return ids;
    }

    void Network::accountProgress(const std::set<TransmissionId>& ids)
    {
        const double now = simulation_.currentTime();
        for (const auto id : ids) {
            auto& active = active_transmissions_.at(id);
            active.remainingBits = std::max(0.0, active.remainingBits
                - active.currentRate * (now - active.lastRateUpdateTime));
            active.lastRateUpdateTime = now;
        }
    }

    double Network::sharedRate(const ActiveTransmission& active) const
    {
        const auto& sender = peer_transport_.at(active.sender);
        const auto& receiver = peer_transport_.at(active.receiver);
        return std::min({peers_[sender.peerIndex].uploadCapacity() / sender.outgoing.size(),
            peers_[receiver.peerIndex].downloadCapacity() / receiver.incoming.size(),
            links_[active.linkIndex].bandwidth()});
    }

    void Network::recomputeRates(const std::set<TransmissionId>& ids)
    {
        // Ordered IDs make replacement-event ordering deterministic.
        for (const auto id : ids) {
            const auto& active = active_transmissions_.at(id);
            const double rate = sharedRate(active);
            const double tolerance = 8 * std::numeric_limits<double>::epsilon()
                * std::max(rate, active.currentRate);
            // Always apply decreases so a skipped comparison cannot exceed a budget.
            if (rate >= active.currentRate && rate - active.currentRate <= tolerance) continue;
            setTransmissionRate(id, rate);
        }
    }

    void Network::setTransmissionRate(TransmissionId id, double newRate)
    {
        const auto found = active_transmissions_.find(id);
        if (found == active_transmissions_.end()) {
            throw std::invalid_argument("Transmission is not active");
        }
        if (!std::isfinite(newRate) || newRate <= 0.0) {
            throw std::invalid_argument("Transfer rate must be finite and positive");
        }
        auto& active = found->second;
        const double now = simulation_.currentTime();
        const double elapsed = now - active.lastRateUpdateTime;
        // At a completion-time boundary, rounding can slightly overshoot the bits left.
        const double remaining = std::max(0.0, active.remainingBits - active.currentRate * elapsed);
        const double completionTime = now + remaining / newRate;
        if (!std::isfinite(completionTime)
            || !std::isfinite(completionTime + links_[active.linkIndex].latency())) {
            throw std::invalid_argument("Transmission completion and arrival times must be finite");
        }

        active.remainingBits = remaining;
        active.lastRateUpdateTime = now;
        active.currentRate = newRate;
        ++active.generation;
        simulation_.schedule(std::make_unique<TransmissionCompleteEvent>(
            completionTime, *this, active));
    }

    void Network::completeTransmission(TransmissionId id, std::uint64_t generation)
    {
        const auto found = active_transmissions_.find(id);
        if (found == active_transmissions_.end() || found->second.generation != generation) return;

        auto affected = affectedTransmissions(found->second.sender, found->second.receiver);
        accountProgress(affected);
        auto active = std::move(found->second);
        active_transmissions_.erase(found);
        peer_transport_.at(active.sender).outgoing.erase(id);
        peer_transport_.at(active.receiver).incoming.erase(id);
        affected.erase(id);
        recomputeRates(affected);
        const auto index = active.linkIndex;
        const auto sender = active.sender;
        auto& link = links_[index];
        simulation_.schedule(std::make_unique<MessageArrivalEvent>(
            simulation_.currentTime() + link.latency(), *this, active.swarmId,
            sender, active.receiver, std::move(active.message), active.lifecycle));
        auto& direction = sender == link.endpointA() ? link.a_to_b_ : link.b_to_a_;
        direction.active = false;
        startTransmission(index, sender);
    }

} // namespace simulator
