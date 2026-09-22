# Qt GUI

The scenario editor stays separate from the runtime engine. Add Swarm creates
file or virtual swarm metadata. The swarm selector, Swarm Info, and message-filter
checkboxes are interactive. Add Peer configures a peer in the active swarm, then
starts canvas placement. Left-click places it; Escape or right-click cancels.
Drag a placed peer or its label to move it; right-click offers Join Swarm/Leave
Swarm and Remove Peer in EDIT. In EDIT, Join/Leave configures initial membership: red means
not initially joined, yellow means joined Leecher, and green means joined Seeder.
Left-click a placed peer to select it and populate the left Inspector; click empty
canvas to clear selection. View Pieces opens a live, scrollable ownership grid.
Block size defaults to 16 KiB and is configurable beside Piece size in Add Swarm
or Swarm Info -> Edit Swarm. Piece geometry is locked after adding peers.
New peers default to not initially joined. Trackers are draggable too, and each
swarm stores its own tracker position for visualization only.
Each swarm retains its own peer and tracker positions. Nodes use the entire visible
canvas without zooming or letterboxing; their complete visual bounds are clamped
at its edges. Resizing a smaller canvas clamps affected positions and stores them.
Bandwidth inputs use whole KiB/s, MiB/s, or GiB/s with exact byte/s storage.
Play validates all swarms and creates one engine runtime session, with real initial
joins queued at t=0. A Qt timer executes one engine step per callback. Pause and
Resume preserve the session. During runtime, peer context menus change current
membership through Join swarm / Leave swarm; inactive peers have no visible
runtime wires. Peers can be dragged while PAUSED to adjust the layout only.
Creating/removing/configuring peers and moving trackers remain EDIT-only.
Swarms skipped at startup remain excluded from runtime Join/Leave. The
Event Log shows up to 2,000 subsequent events for the selected peer. Next Event steps once while paused. Stop preserves completed pieces and membership, destroys the runtime, and returns to EDIT at time zero. See [RUNTIME.md](RUNTIME.md) for bridge ownership,
identity, piece generation, link defaults and limitations.

Configure with a Qt 6 Widgets kit matching your compiler:

    cmake -S . -B cmake-build-gui -DPICOTORRENT_BUILD_GUI=ON -DCMAKE_PREFIX_PATH="<Qt kit directory>"
    cmake --build cmake-build-gui --target picoTorrent_gui

Launch the executable in the build directory's gui subdirectory (or the platform
application bundle). On Windows, use the Qt kit's runtime environment or deploy
its DLLs and platform plugin with that kit's windeployqt tool.

PICOTORRENT_BUILD_GUI defaults to OFF, so existing headless builds do not require Qt.
Requesting the GUI without Qt 6 Widgets produces a required-package CMake error.

The tracker image is embedded through resources.qrc. File selection reads only
file metadata; it does not read or hash contents. Scenario state is held in memory.

Import and Export beside Settings load/save versioned JSON .pt projects in EDIT.
They preserve exact ownership, membership, settings, metadata and positions, not
a running session. See [SCENARIO_FORMAT.md](SCENARIO_FORMAT.md) for the schema.

During runtime, select a peer to reveal its directional wires, then click a wire
or its packet icon to inspect the physical link in the existing Inspector.
The existing message-type icon marks each enabled active direction (up to two per link); queued
messages and post-transmission propagation have no icon. Click an active message
name in the Inspector while paused to open the same popup style as a log row,
including modeled payload, wire size, rate, remaining bits and completion prediction.
The Inspector stays compact. Clicking a peer restores peer inspection.
The Message Filter checkboxes control both link icons and active-message Inspector
entries immediately.

The Event Log starts empty for each selected peer and records only subsequent
events involving that peer. Message Filter controls future SEND (actual start)
and RECEIVE (actual arrival) rows. Rechokes, completed pieces/downloads and
membership transitions remain visible regardless of message filters.
Click a message row while paused for its captured modeled fields; running
playback does not open inspection. Rows are capped at 2,000 and inserted in
50 ms presentation batches, flushed immediately on pause/step. No engine time
or event scheduling is changed. Verification failures are not modeled.
