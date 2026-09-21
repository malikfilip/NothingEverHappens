# First runtime bridge

Play creates one RuntimeSession for every swarm with at least one initially joined
peer, independent of the toolbar selection. A zero-participation scenario is an
error. One consolidated confirmation lists single-peer swarms, participating
swarms without an initially joined Leecher, and skipped swarms. Cancel performs
no generation or engine work. Invalid engine sizes, IDs, counts, inventories,
capacities, or positions are rejected with errors before startup.

RuntimeSession owns Simulation and a Network. Network owns its engine Swarms,
Peers, Links and Tracker. It is destroyed before Simulation. Session copying and
moving are disabled because engine events and Network refer to these objects.
MainWindow owns only the session pointer, rather than individual engine objects.
The engine exposes a read-only nextEventTime() peek; engine behavior is unchanged.

## Identity and piece ownership

The session maps stable 64-bit scenario swarm IDs to dense 32-bit engine SwarmIds.
Each peer binding is keyed by its stable scenario peer ID and contains its owning
scenario swarm ID, engine SwarmId, engine PeerId, frozen normalized position and
initial bitfield. Names are labels, never identity. Every peer in a participating
swarm is materialized, including red peers; skipped swarms are not materialized.

The engine's 20-byte InfoHash is supplied as a **simulation-only identity**, not a
real torrent SHA-1 hash: ASCII `PICO-SIM-SW`, a version byte of 1, and the full
8-byte scenario swarm ID in big-endian order. PeerProtocolId similarly uses
`PICO-SIM-PR`, version 1, and the scenario peer ID. No files are read or hashed.
These identities are deterministic, names/configuration independent and isolated
by scenario identity. They are not suitable for interoperability with real torrents.

ScenarioPeer.initialBitfield is optional packed ownership. Absence means ownership
has not been generated; an allocated zero bitfield means no pieces. Piece i uses
bit `0x80 >> (i % 8)` in byte `i / 8`; spare bits are zero. Seeders get every piece;
Leechers get exactly initialPieceCount distinct pieces. Floyd sampling selects a
uniform subset using mt19937_64, seed_seq of simulation seed + stable swarm/peer
IDs, and explicit rejection sampling. Dense inventories sample their missing
pieces instead. Default scenario seed is 0, matching the engine default; no seed
UI is introduced. Streams are independent of iteration order and tracker RNG.
Concrete ownership is reused once present, even if another seed is requested.
The same ScenarioPieces helper now prepares ownership when adding a GUI peer,
so EDIT inspection has a retained concrete inventory. Older unprepared scenarios
are still prepared transactionally at startup. Inspection never generates pieces.

Startup stages a scenario copy and a fresh session. Only after all construction
and initial joins succeed is the generated ownership committed to the editor.
Failures leave original scenario records unchanged. No refresh generates pieces.

## Initial lifecycle and links

Only initiallyJoined peers enter Network::joinSwarm, with JoinOptions.initialBitfield.
This marks engine membership active and schedules the existing STARTED event at
simulation time zero. Construction does not execute queued events. Play starts the Qt event pump; real
registration/discovery occurs when it executes the queued STARTED events.
No manual tracker registration, handshakes, or protocol logic exist in the bridge.

Never-joined red peers have no engine PeerSwarmState yet. Their inventory is kept
in the scenario and session binding for a later first join. Creating inactive
membership directly is not a public engine operation; this bridge deliberately
does not fake a join/leave cycle. Future first joins must supply that inventory;
rejoins must omit initialBitfield to preserve the engine's downloaded data.

Positions are frozen at Play. For the visible canvas rectangle, the bridge uses
`((x-left)/width, (y-top)/height)`, clamped to [0,1] for saved off-canvas positions.
The LinkConfigProvider captures these normalized positions by value. Euclidean
distance divided by sqrt(2) maps linearly to 0.001..0.050 simulation seconds.
Default physical-link bandwidth is 100 MiB/s (838860800 bits/s). Peer byte/s rates
are converted to the engine's double bits/s; the scenario keeps exact integers.
Tracker position has no role. No Link is created until engine discovery admits
an attempt, and no Links are drawn by the GUI. Resizing after Play cannot change
provider geometry.

## Current UI and engine limits

Successful startup enters runtime mode, disables Play/Add Swarm/Add Peer and
canvas editing/context actions, and preserves viewing/swarm selection/help.
The title reports RUNNING or PAUSED. Stop snapshots completed pieces and membership, destroys the session, and returns to EDIT. Next Event executes one queued event while paused. Pause and
Resume control the event pump. Closing the application releases the session;
Stop creates no engine lifecycle events; the next Play constructs a fresh session.

Engine piece length/count and numeric runtime IDs are uint32_t, while the editor
allows larger sizes/IDs. Oversized piece lengths/counts reject; scenario IDs are
mapped without narrowing. Large bitfields can still exhaust memory; startup
exceptions are reported without publishing partial scenario state. Capacities
use double bits/s in the engine, so very large exact integers may be rounded.
## Validation

With PICOTORRENT_BUILD_GUI and BUILD_TESTING enabled, runtime_session_tests links
Qt Core and the bridge, with no widgets, display server or GUI interactions.
It tests preflight/transaction rejection, mappings, exact inventories, deterministic
sampling, untouched queued startup, real STARTED registration and lazy links,
including geometry invariance under proportional size/origin changes.
The existing seven engine tests remain registered and unchanged.

## Continuous execution and log

RuntimePump is a Qt Core controller owned by MainWindow, destroyed before its
RuntimeSession. EDIT -> successful validation/session creation -> RUNNING;
RUNNING -> Pause -> PAUSED; PAUSED -> Resume -> RUNNING. Validation cancellation
or failure leaves EDIT. Stop returns to EDIT after snapshotting completed ownership and membership.
Structural editing stays locked in both runtime states.

A single-shot precise QTimer executes at most one Simulation::step() per timeout
and returns control to Qt. Playback defaults to 1x, with 0.5x/2x/5x/10x and Max.
At speed S, a GUI-only virtual playback position advances S simulated seconds per
monotonic wall second. The next engine timestamp is due when that position reaches
it. Absolute anchors retain timer lateness credit, avoiding per-event drift.
Remaining delays round up to milliseconds and are capped at one second before
integer conversion; wakeups recheck deadlines. Equal-time events need no added delay.
Max uses the original zero-delay callbacks with a 1 ms yield every 64 steps;
that same yield bounds due-event bursts at paced speeds. No engine sleeps or threads.
Pause freezes the playback position, including partial progress toward a pending
event. Resume excludes paused wall time. Running speed changes preserve accrued
progress and replace the pending timer; leaving Max anchors at actual engine time.
No speed change resets the RuntimeSession or mutates engine timestamps.
Pause stops the timer and changes the state checked by each callback. All work
is on the Qt thread: an atomic step finishes before a user's Pause is handled;
no later step runs while paused. Resume retains the same session, queue, and time.
A false step result enters PAUSED and stops scheduling, retaining the session.
An exception also pauses and displays the error; failed events are not rolled back.

The two-column Event Log copies event.time() and traceDescription() directly
from Simulation's pre-execution observer, including synchronous executeNow
notifications in dispatch order. It does not imply post-event success or inspect
post-event state. Stale/no-op events may therefore appear. The log clears on Stop and new session creation, preserves entries across Pause/Resume, and scrolls to
the bottom after each step. It retains at most 2,000 rows, removing the oldest
GUI row before appending; the engine is unaffected. Toolbar time displays GUI playback time as total minutes:ss.mmm. It advances between events while running and freezes on Pause. Message Filter remains disconnected.

runtime_pump_tests uses QCoreApplication without widgets or GUI interaction to
exercise real recurring tracker events, bounded callbacks, pause/resume identity
and time, observer ordering including immediate events, empty queues and errors.
Future visualization must account for pre-execution notifications, stale events,
and multiple synchronous notifications inside one indivisible step. One complex
engine event can still take appreciable wall time; the pump cannot interrupt it.

## Block configuration and peer inspection

ScenarioSwarm retains exact blockSizeBytes plus its display value/unit, defaulting
to 16 KiB. Add Swarm and Swarm Info -> Edit Swarm validate a positive block size
no larger than the piece size. Editing after peers exist locks torrent/piece
geometry to preserve concrete inventories; block size and name remain editable.
RuntimeSession passes custom sizes to Swarm's explicit constructor. Legacy models
with tiny pieces and the unchanged 16 KiB default retain their old clipping behavior.

Left-click selection belongs to SimulationView and uses stable scenario IDs.
The existing Inspector/filter sidebar is on the left. Empty canvas clicks, removal,
and swarm switches clear selection; right-click menus and editing drags are retained.
PeerInspection takes a read-only snapshot from ScenarioPeer in EDIT, or from the
mapped engine Peer and Swarm during runtime. Never-joined peers use the retained
binding inventory; skipped swarms have no runtime mapping. No engine accessors
were needed. Counts, completion, capacities, membership, connection/handshake,
choke/interest and request counts use existing authoritative state.

MainWindow refreshes after each completed pump step (and after errors), not from
the pre-execution observer. The nonmodal View Pieces dialog follows selection and
updates while visible. Its row-scrolled grid paints only visible cells, with no
per-piece widgets and zero-based index/ownership tooltips. Clearing selection hides
it. Old scenarios lacking concrete inventory show an explicit unavailable message
until prepared; opening the dialog never randomizes ownership.

## Simulation settings

The last toolbar action opens a modal Simulation Settings dialog in EDIT mode.
MainWindow owns scenario-wide ScenarioSettings, separate from the swarm list.
Its BitTorrent settings default to regular rechoke = 10 s and optimistic unchoke
= 30 s. Both must be finite and positive, with optimistic >= regular; no
integer-multiple constraint applies. The dialog supports 0.001..86400 seconds
with millisecond precision and commits only on valid OK. Cancel discards edits.

RuntimeSession validates and stores an immutable copy at creation. Settings are
disabled whenever a session exists, including pause/error states. Stop restores Settings in EDIT. The shared engine BitTorrentSettings type reaches Network through
RuntimeSession and controls bootstrap plus peer/swarm-local choking deadlines.
For regular=5, first actionable interest at T immediately fills available upload
slots, with full preferred decisions at T+5, T+10, T+15. Optimistic timing is
independent (for example 5/25). Simulation has no BitTorrent-specific fields.

## Continuous playback display

The GUI-only PlaybackClock advances with QElapsedTimer at the selected speed.
RuntimePump refreshes displayed Sim Time on a dedicated 16 ms presentation timer,
clamped between current engine time and the next queued event. Simulation::step()
runs only once the paced clock reaches that timestamp. Max retains rapid event
processing with periodic UI yields. No display update creates an engine event
or an Event Log entry; Simulation::currentTime() remains discrete engine time.

Pause freezes playback progress; resume retains progress toward the same event.
Next Event is enabled only while paused with a queued event. It executes exactly
one step (preserving equal-time ordering), resets playback to that event's time,
and stays paused. Empty queues and execution errors leave playback paused.
## Download completion notifications

Network dispatches PeerCompletedEvent synchronously through Simulation::executeNow
when a newly owned piece makes the receiver own every piece. It carries actual
engine time and peer/swarm IDs and appears as PEER_COMPLETED in the existing log.
It changes no protocol state and adds no queued event. Already-complete seeds
and retired/duplicate PIECEs do not generate completion notifications.

MainWindow queues names from these notifications, refreshes the existing peer
colors after the atomic step, and pauses before showing one asynchronous modal
popup at a time. Continue (or closing the popup) resumes only after the queue is
empty and only if the run was active before presentation. Completion reached by
paused Next Event stays paused. Stop Simulation invokes the same MainWindow::stopSimulation() as toolbar Stop.

## Stop and fresh Play

MainWindow::stopSimulation is the only Stop lifecycle, shared by toolbar Stop and
completion-popup Stop. It first pauses the pump, then transactionally snapshots
RuntimeSession into the scenario. RuntimePump::stop cancels its timers, detaches
the engine pointer and resets playback to zero before the session is destroyed.
The completion timer, queued messages, open popup, resume intent and Event Log
are cleared. Canvas editing, Settings and add controls return; colors and Inspector
refresh from the scenario. Stop with no session does nothing. If snapshotting
fails, the scenario is unchanged and the old session remains paused for retry.

Only each peer's exact localBitfield and active membership survive. Saved inactive
memberships retain their completed pieces; never-joined peers retain their initial
bitmap. Skipped swarms remain unchanged. Existing editor count and role fields
are derived from the copied bitmap to keep preflight consistent; bitmap ownership
remains the source of truth. Positions, names, capacities and settings are retained.

No receivedBlocks are copied: an incomplete piece loses all partial block progress.
Time, event queue, tracker state, connections/links, transfers, request reservations
and choking assignments/deadlines disappear with RuntimeSession. Next Play creates
a new session at t=0 with fresh STARTED events and exact saved completed pieces.
