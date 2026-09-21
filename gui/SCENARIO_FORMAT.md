# picoTorrent scenario files (.pt)

A .pt file is UTF-8, indented JSON describing the editable picoTorrent project.
It is not a running simulation snapshot. Import and Export sit beside Settings
and are available only in EDIT. The file dialogs use "picoTorrent Scenario (*.pt)"
and default to the .pt extension when no extension is entered.

The root object contains:

- `format`: "picoTorrent"
- `version`: 1 (unsupported versions are rejected)
- `seed`: simulation seed, as an unsigned decimal string
- `settings.bitTorrent`: `regularRechokeInterval` and
  `optimisticUnchokeInterval`, in simulated seconds
- `swarms`: an ordered array of scenario swarms

Each swarm stores all ScenarioSwarm fields: stable ID, name, file/virtual mode,
file path, total/piece/block byte sizes, piece count, tracker position, original
size display values/units, and an ordered `peers` array. File paths are metadata:
Import neither reads the source payload file nor requires it to exist.

Each peer stores all ScenarioPeer fields: stable ID, name, initial role and piece
count, membership (`initiallyJoined`), upload/download capacities in bytes/second,
capacity display values/units, position, and `initialBitfield`.

IDs, seed, byte quantities and counts are **decimal JSON strings**, preserving
the entire uint64 range even in JSON readers based on floating-point numbers.
Intervals and position coordinates are JSON numbers; membership is a boolean.
All fields written by version 1 are required when reading it; unknown additional
fields are ignored. Modes are "file"/"virtual"; roles are "seeder"/"leecher".

Ownership is a packed hexadecimal string. Piece i is bit 0x80 >> (i % 8) of byte
i / 8; unused trailing bits must be zero. For six pieces, ownership {0,2,4} is
`"initialBitfield": "a8"`. An all-zero string is concrete empty ownership.
A null bitfield preserves an older unprepared scenario; its stored role/count,
IDs and seed determine the same inventory when Play prepares it.

Positions are `{"x": number, "y": number}` in the scenario's existing centered
scene coordinates (the current model uses unscaled scene units). They are saved
unchanged; this format introduces no coordinate conversion. Existing canvas
edge-clamping still applies if a project is viewed in a smaller viewport.
RuntimeSession's normalized positions are runtime-only and are not serialized.

Import parses and validates a temporary project before replacing the editor.
It checks format/version, required fields and types, finite coordinates/settings,
unique nonzero IDs, sizes/piece geometry, positive capacities, membership flags,
and bitmap length/count/padding using the shared scenario invariants.
Empty projects and unjoined swarms are valid EDIT projects; Play separately
requires participation. Malformed data leaves the current project untouched.
Export validates first and uses QSaveFile for atomic replacement.

After Import the editor resets selection and log, displays time zero and rebuilds
the swarm selector/canvas. Play always creates a fresh RuntimeSession. No event
queue, simulation time, partial blocks, requests, reservations, choking state,
tracker registrations, runtime links/transfers, completion notifications or
Event Log is stored. To save downloaded completed pieces, Stop first: Stop
snapshots exact completed ownership and membership into the scenario, which
Export then preserves.
