#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/SendMessageEvent.hpp"
#include "simulator/Simulation.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
#include "simulator/TransmissionStartEvent.hpp"

using namespace simulator;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
bool sameTime(double a, double b) { return std::abs(a - b) < 1e-9; }
bool overlaps(const RequestPayload& a, const RequestPayload& b)
{
    return a.index == b.index && a.begin < b.begin + b.length && b.begin < a.begin + a.length;
}

void autonomousFourPeerRing(bool tracing)
{
    constexpr double bandwidth = 1000000;
    constexpr double latency = .01;
    constexpr std::uint32_t pieceLength = 12 * 16384 + 123;
    const Swarm swarm(1, InfoHash{0x71}, 3 * pieceLength + 5 * 16384 + 37, pieceLength);
    // Ring 1--2--3--4--1. Neither initial source owns the full swarm.
    const std::array<std::uint8_t, 5> initial{0, 0xc0, 0, 0x30, 0};
    std::vector<Peer> peers;
    for (PeerId id = 1; id <= 4; ++id) {
        peers.emplace_back(id, bandwidth, bandwidth, PeerProtocolId{static_cast<std::uint8_t>(id)});
        peers.back().joinSwarm(swarm, {initial[id]});
    }
    Simulation simulation(tracing);
    Network network(simulation, peers,
        {Link(1, 2, bandwidth, latency), Link(2, 3, bandwidth, latency),
         Link(3, 4, bandwidth, latency), Link(4, 1, bandwidth, latency)}, {swarm});
    struct Enqueued { MessageType type; double time; };
    struct Transfer { Message message; double completion; double arrival; };
    struct Direction {
        std::deque<Enqueued> queued;
        std::optional<Transfer> active;
        std::deque<Transfer> arrivals;
        double lastCompletion = 0;
        std::array<unsigned, 10> sends{};
        unsigned starts = 0, completions = 0, delivered = 0;
    };
    std::array<std::array<Direction, 5>, 5> directions;
    std::array<std::vector<RequestPayload>, 5> requestHistory;
    std::array<std::array<bool, 5>, 5> usedRemote{};
    std::array<std::array<std::size_t, 5>, 5> maxPending{};
    std::array<std::array<std::uint8_t, 5>, 5> learnedFromHave{};
    std::array<std::array<bool, 5>, 5> interestGainArmed{};
    std::array<bool, 5> concurrentReception{};
    std::array<unsigned, 5> relayedBlocks{};
    unsigned haveInterestGains = 0;
    bool independentLinks = false, fullDuplex = false;

    simulation.setEventObserver([&](const Event& event) {
        std::array<double, 5> outgoing{}, incoming{};
        for (const auto& [id, active] : network.activeTransmissions()) {
            outgoing[active.sender] += active.currentRate;
            incoming[active.receiver] += active.currentRate;
        }
        for (PeerId id = 1; id <= 4; ++id) {
            require(outgoing[id] <= bandwidth + 1e-8 && incoming[id] <= bandwidth + 1e-8,
                "Aggregate active bandwidth exceeded peer capacity");
        }
        // Inspect actual reservations at every event boundary, not just transmitted requests.
        for (PeerId id = 1; id <= 4; ++id) {
            const auto& state = network.peer(id).swarmState(1);
            std::vector<RequestPayload> occupied;
            for (const auto& [remote, connection] : state.connections) {
                const auto pending = connection.scheduledRequests.size() + connection.outgoingRequests.size();
                require(pending <= 5, "Per-connection pipeline exceeded depth five");
                maxPending[id][remote] = std::max(maxPending[id][remote], pending);
                for (const auto* requests : {&connection.scheduledRequests, &connection.outgoingRequests}) {
                    for (const auto& request : *requests) {
                        for (const auto& earlier : occupied) require(!overlaps(request, earlier), "Reservations overlap across remotes");
                        if (const auto received = state.receivedBlocks.find(request.index); received != state.receivedBlocks.end()) {
                            for (const auto& range : received->second) {
                                require(!overlaps(request, {request.index, range.begin, range.end - range.begin}),
                                    "Request overlaps already received data");
                            }
                        }
                        occupied.push_back(request);
                    }
                }
            }
        }
        if (const auto* send = dynamic_cast<const SendMessageEvent*>(&event)) {
            const auto details = send->details();
            require(details.swarmId == 1, "Message escaped the integration swarm");
            auto& direction = directions[details.sender][details.receiver];
            direction.queued.push_back({details.messageType, event.time()});
            ++direction.sends[static_cast<int>(details.messageType) + 1];
            if (details.messageType == MessageType::Interested && interestGainArmed[details.sender][details.receiver]) {
                require(network.peer(details.sender).swarmState(1).connections.at(details.receiver).weAreInterestedInRemote,
                    "HAVE did not make the new source interesting");
                ++haveInterestGains;
                interestGainArmed[details.sender][details.receiver] = false;
            }
        } else if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event)) {
            const auto details = start->details();
            auto& direction = directions[details.sender][details.receiver];
            require(!direction.active && !direction.queued.empty(), "Transmission bypassed its directional FIFO");
            const auto queued = direction.queued.front();
            direction.queued.pop_front();
            require(queued.type == details.messageType, "Directional FIFO changed message order");
            require(sameTime(event.time(), std::max(queued.time, direction.lastCompletion)),
                "Another link or propagation latency delayed transmission start");
            const auto& message = start->message();
            const double completion = std::numeric_limits<double>::infinity(); // Rate can change while active.
            if (message.type() == MessageType::Request) {
                const auto block = std::get<RequestPayload>(message.payload());
                require(block.length > 0 && block.length <= 16384 && block.begin + block.length <= swarm.pieceSize(block.index),
                    "Automatic request has invalid block bounds");
                for (const auto& earlier : requestHistory[details.sender]) {
                    require(!overlaps(block, earlier), "A peer requested duplicate bytes, possibly from another remote");
                }
                require((initial[details.sender] & (0x80u >> block.index)) == 0, "Requested an initially owned piece");
                if ((initial[details.receiver] & (0x80u >> block.index)) == 0) {
                    require((learnedFromHave[details.sender][details.receiver] & (0x80u >> block.index)) != 0,
                        "Requested a relayed piece before learning availability through HAVE");
                }
                requestHistory[details.sender].push_back(block);
                usedRemote[details.sender][details.receiver] = true;
            }
            if (message.type() == MessageType::Piece) {
                const auto block = std::get<PiecePayload>(message.payload());
                if ((initial[details.sender] & (0x80u >> block.index)) == 0) {
                    const auto& state = network.peer(details.sender).swarmState(1);
                    require(state.receivedBlocks.at(block.index) == std::vector<BlockRange>{{0, swarm.pieceSize(block.index)}},
                        "Peer uploaded a relayed piece before receiving full coverage");
                    ++relayedBlocks[details.sender];
                }
                for (PeerId other = 1; other <= 4; ++other) {
                    const auto& incoming = directions[other][details.receiver].active;
                    if (other != details.sender && incoming && incoming->message.type() == MessageType::Piece
                        && incoming->completion > event.time()) concurrentReception[details.receiver] = true;
                }
            }
            for (PeerId a = 1; a <= 4; ++a) {
                for (PeerId b = 1; b <= 4; ++b) {
                    const auto& active = directions[a][b].active;
                    if (!active || active->completion <= event.time()) continue;
                    if (a == details.receiver && b == details.sender) fullDuplex = true;
                    else if (a != details.sender || b != details.receiver) independentLinks = true;
                }
            }
            direction.active = Transfer{message, completion, completion + latency};

            ++direction.starts;
        } else if (const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
            const auto active = network.activeTransmissions().find(complete->transmissionId());
            if (active == network.activeTransmissions().end()
                || active->second.generation != complete->generation()) return;
            const auto& transmission = active->second;
            require(sameTime(event.time(), transmission.lastRateUpdateTime
                + transmission.remainingBits / transmission.currentRate), "Incorrect shared-rate completion time");
            const auto details = *complete->details();
            auto& direction = directions[details.sender][details.receiver];
            require(direction.active && direction.active->message.type() == details.messageType,
                "Completion does not match the active transmission");
            direction.active->completion = event.time();
            direction.active->arrival = event.time() + latency;
            direction.arrivals.push_back(*direction.active);
            direction.active.reset();
            direction.lastCompletion = event.time();
            ++direction.completions;
        } else if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event)) {
            const auto details = arrival->details();
            auto& direction = directions[details.sender][details.receiver];
            require(!direction.arrivals.empty(), "Arrival without a transmission");
            const auto transfer = direction.arrivals.front();
            direction.arrivals.pop_front();
            require(transfer.message.type() == details.messageType && sameTime(event.time(), transfer.arrival),
                "Arrival order or propagation timing changed");
            if (details.messageType == MessageType::Have) {
                const auto piece = std::get<HavePayload>(transfer.message.payload()).pieceIndex;
                const auto mask = 0x80u >> piece;
                const auto& state = network.peer(details.receiver).swarmState(1);
                const auto& connection = state.connections.at(details.sender);
                learnedFromHave[details.receiver][details.sender] |= static_cast<std::uint8_t>(mask);
                if ((state.localBitfield[0] & mask) == 0 && !connection.weAreInterestedInRemote) {
                    interestGainArmed[details.receiver][details.sender] = true;
                }
            }
            ++direction.delivered;
        }
    });
    // Only handshakes are injected. All subsequent protocol traffic is autonomous.
    for (const auto [sender, receiver] : {std::pair{2u, 1u}, std::pair{2u, 3u}, std::pair{4u, 1u}, std::pair{4u, 3u}}) {
        simulation.schedule(std::make_unique<SendMessageEvent>(0, network, 1, sender, receiver,
            Message(MessageType::Handshake, HandshakePayload{swarm.infoHash(), peers[sender - 1].protocolId()})));
    }
    simulation.run();
    require(independentLinks && fullDuplex, "Scenario did not exercise independent links and full duplex");
    require(concurrentReception[2] && concurrentReception[4], "Leechers did not receive concurrent PIECE transfers from multiple sources");
    require(relayedBlocks[2] > 0 && relayedBlocks[4] > 0 && haveInterestGains >= 2,
        "Downloaded pieces did not become useful sources via HAVE and subsequent uploads");
    for (PeerId id = 1; id <= 4; ++id) {
        const auto& state = network.peer(id).swarmState(1);
        require(state.localBitfield == std::vector<std::uint8_t>{0xf0}, "A peer did not obtain every available piece");
        require(state.connections.size() == 2, "Ring topology changed");
        std::uint64_t expectedBytes = 0, requestedBytes = 0;
        for (std::uint32_t piece = 0; piece < swarm.pieceCount(); ++piece) {
            if ((initial[id] & (0x80u >> piece)) == 0) {
                expectedBytes += swarm.pieceSize(piece);
                require(state.receivedBlocks.at(piece) == std::vector<BlockRange>{{0, swarm.pieceSize(piece)}}, "Incomplete merged piece coverage");
            }
        }
        for (const auto& block : requestHistory[id]) requestedBytes += block.length;
        require(requestedBytes == expectedBytes, "Requested byte coverage differs from initially missing data");
        unsigned sources = 0;
        for (const auto& [remote, connection] : state.connections) {
            sources += usedRemote[id][remote];
            require(connection.handshakeComplete() && connection.bitfieldSent, "Handshake/bitfield flow incomplete");
            require(connection.remoteBitfield == state.localBitfield, "Final remote availability is stale");
            require(connection.scheduledRequests.empty() && connection.outgoingRequests.empty() && connection.acceptedRequests.empty(),
                "Requests remain after swarm completion");
            require(!connection.weAreInterestedInRemote && !connection.remoteInterestedInUs
                && connection.weAreChokingRemote && connection.remoteIsChokingUs, "Final interest/choke state is inconsistent");
            const auto& direction = directions[id][remote];
            require(direction.queued.empty() && !direction.active && direction.arrivals.empty(), "Transport did not drain");
            require(direction.starts == direction.completions && direction.starts == direction.delivered, "Transport event counts differ");
            require(direction.sends[static_cast<int>(MessageType::Handshake) + 1] == 1
                && direction.sends[static_cast<int>(MessageType::Bitfield) + 1] == 1, "Initial exchange duplicated or missing");
            require(direction.sends[static_cast<int>(MessageType::Have) + 1] == 4 - std::popcount(static_cast<unsigned>(initial[id])),
                "HAVE count differs from first-time piece completions");
            const auto transport = network.transmissionState(id, remote);
            require(!transport.active && transport.pendingCount == 0, "Network direction remains busy");
        }
        require(sources == 2, "A peer did not download from both connected remotes");
    }
    require(maxPending[2][1] == 5 && maxPending[2][3] == 5
        && maxPending[4][1] == 5 && maxPending[4][3] == 5, "Scenario never filled the concurrent pipelines");
    std::cout << "PASS: four-peer autonomous ring, all four pieces complete at t=" << simulation.currentTime() << '\n';
}
} // namespace

int main(int argc, char* argv[])
{
    try {
        autonomousFourPeerRing(argc > 1 && std::string_view(argv[1]) == "--trace");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: four-peer autonomous ring: " << error.what() << '\n';
        return 1;
    }
}
