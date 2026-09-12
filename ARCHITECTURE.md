# picoTorrent Architecture

## Project goal

picoTorrent is a single-process, discrete-event BitTorrent simulator written in C++20.

The simulator models BitTorrent behavior at the application level.
TCP/IP packet-level behavior is abstracted through link bandwidth and latency.

The GUI is a visualization layer and must not contain protocol logic.

---

## Simulation model

The simulator is single-threaded and event-driven.

Simulation time is logical/simulated time, not wall-clock time.

Events are stored and executed in chronological order.
Events with the same timestamp use a sequence number for deterministic ordering.

Current important event types:

- SendMessageEvent
- TransmissionStartEvent
- TransmissionCompleteEvent
- MessageArrivalEvent

Semantics:

SendMessageEvent
= a peer requests that a message be transmitted / the message is enqueued.

TransmissionStartEvent
= the message actually begins consuming bandwidth.

TransmissionCompleteEvent
= transmission has finished and the directional link may start the next queued message.

MessageArrivalEvent
= the message reaches the receiving peer after propagation latency.

---

## Network model

Network knows:

- Peers
- Links
- Swarms

A Link represents an abstract end-to-end path between two peers.

Each Link is full-duplex and has two independent directional transmission states:

A -> B
B -> A

Each direction has a FIFO queue.

Messages in the same direction are serialized.
Opposite directions may transmit simultaneously.

Effective bandwidth currently is:

min(
sender upload capacity,
receiver download capacity,
link bandwidth
)

Transmission time:

message.wireSize() * 8 / effectiveBandwidth

Arrival time:

transmission start time
+ transmission time
+ link latency

Propagation latency does not keep the directional link occupied.

---

## Identifiers

Simulator identifiers and BitTorrent protocol identifiers are separate.

SwarmId
= uint32_t
= internal simulator identifier

InfoHash
= 20-byte BitTorrent info_hash

PeerId
= uint32_t
= internal simulator identifier

PeerProtocolId
= 20-byte BitTorrent peer_id

Do not invent conversions between simulator IDs and protocol IDs.

---

## Swarm

Swarm currently contains:

- SwarmId
- InfoHash
- pieceCount

pieceCount belongs to Swarm metadata and is not duplicated in Peer state.

---

## Peer swarm state

A Peer may participate in multiple swarms.

Conceptually:

Peer
-> SwarmId
-> PeerSwarmState
-> localBitfield
-> remote PeerId
-> PeerConnectionState

PeerConnectionState currently tracks protocol state such as:

- handshakeSent
- handshakeReceived
- bitfieldSent
- remoteChokingUs
- remoteInterestedInUs
- remoteBitfield

Handshake completion is derived from:

handshakeSent && handshakeReceived

---

## Bitfield

Bitfields are packed using:

std::vector<std::uint8_t>

One bit represents one piece.

Piece 0 is the high bit of byte 0, following BEP 3.

For piece index i:

byteIndex = i / 8
bit mask = 0x80 >> (i % 8)

Unused trailing bits must be zero.

---

## Messages

Message payloads use std::variant.

Implemented message types include:

- Handshake
- Choke
- Unchoke
- Interested
- NotInterested
- Have
- Bitfield
- Request
- Piece
- Cancel

Piece payloads represent block length but do not store fake block data.

Message::wireSize() models BEP 3 application-level message size.
TCP/IP headers are not explicitly simulated.

---

## Handshake lifecycle

Handshake validation checks:

- Swarm InfoHash
- actual sender PeerProtocolId

A connection is complete only when both:

- handshakeSent == true
- handshakeReceived == true

After local handshake completion, the peer automatically sends its BITFIELD once.

---

## GUI / tracing

The simulation engine is the source of truth.

Console tracing and the future Qt GUI should observe engine events rather than implement protocol behavior themselves.

For visualization:

- TransmissionStartEvent is the moment a message should begin moving in the GUI.
- MessageArrivalEvent is the moment it reaches the receiver.