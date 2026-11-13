# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Player input, from the client to the server. A new message, `Input`, carries up to 4 inputs (the newest and the 3
  before it), each with a sequence number and up to 16 bytes that the game defines. The server hands each new input to
  the game once and in order (`IGameLogic::OnPlayerInput`, `ReplicationServer::SetInputHandler`), drops repeats and
  late ones, and limits how many a player can have handled per tick (`ServerConfig::max_inputs_per_tick`, 8 by
  default). `ServerStats` counts the inputs delivered, repeated, old and dropped; `ClientStats` counts the inputs sent and
  refused. The client sends with `ClientReplica::SendInput`, which gives the sequence number, or 0 when nothing was sent.
- In the Godot extension, `TesseraClient.send_input(bytes)` and `TesseraClient.get_connection_id()`.
- The example server started with `--players` makes one entity for each connected player (type 2, with a position and
  the owner's connection id), moves it with the two input bytes it receives, keeps the player's observer with it and
  removes it when the player leaves. Without `--players` nothing changes, the schema hash included.
- In the demo project, the player's own sphere (green; the other players' are orange) moves with W, A, S and D. There is
  no prediction yet: the sphere moves one round trip after the key press.
- Tests: the default build now has 537 tests (it had 495), with a test of the input against a simulated network that loses
  30 percent of what the client sends; the server tests, the extension tests and the headless Godot test cover the
  example's players and the two new calls.
- Architecture decision record 0017, "Player input" (Proposed).

### Changed
- **The protocol version is now 2.** Clients and servers of 0.2.0-alpha no longer connect to the new ones: the server
  refuses their hello with `VersionMismatch`. The bytes of the messages that already existed did not change.

## [0.2.0-alpha] - 2026-11-13

### Added
- A first Godot client: the GDExtension `tessera_net` (in `extension/`) with `TesseraSchema`, which describes the types of
  entity and their properties in GDScript, and `TesseraClient`, which connects to a server over ENet, is read with
  `poll()`, and tells a script about entities with signals. It is built only with `-DTESSERA_BUILD_EXTENSION=ON` (off by
  default; with it off nothing is fetched and the 495 tests are unchanged), which fetches godot-cpp at one pinned commit
  and needs the ENet and server options. ENet is linked into the library with its symbols hidden, and a test fails if the
  library exports anything but its entry point. The code that does not need Godot is its own library with its own tests
  (`tessera_extension_tests`). It has been loaded into Godot 4.7.2 on Windows and tested end to end in a headless Godot
  against the example server; on Linux the same test was run locally, in a container, with a Godot built from source.
  The CI workflow does not run it, because there is no Linux Godot binary we could pin and verify.
- Presets `windows-msvc-extension-debug`, `windows-msvc-extension-release`, `linux-extension-debug` and
  `linux-extension-release`, and two jobs in the CI workflow that build the extension and run its tests on Ubuntu (GCC
  and Clang) and Windows (MSVC). They pass in GitHub Actions.
- A demo project, `examples/godot-client-demo`, the guide "Using the client in Godot" and architecture decision record
  0016.
- The dedicated server as a library, `tessera_server` (in `server/`), built only with `-DTESSERA_BUILD_SERVER=ON`
  (off by default; with it off the build and the 495 tests are unchanged). Settings from a file and from arguments
  with clear errors, a fixed-rate tick loop (`ServerHost`) around a game that implements `IGameLogic`, a clean stop that
  is safe from a signal handler, a log with time stamps and levels, an optional summary line (tick, players, bytes
  per second) and a `shard_id` that reaches the entity ids.
- `tessera_example_server`, a small game on a real UDP port over ENet, as the model for your own server, and tests for it:
  the library on a simulated network, the example over ENet in loopback, and a script that starts it as a process and
  sends it signals.
- The `windows-msvc-server` preset, Linux presets (`linux-debug`, `linux-server-*` and `linux-enet-*`), and a CI
  workflow that builds and tests Ubuntu (GCC and Clang) and Windows (MSVC), in Release and Debug, with and without ENet,
  with warnings as errors.
- A section "Running a server" in the networking reference, `server/README.md` and architecture decision record 0014.
- A section about running the checks of the CI locally in `CONTRIBUTING.md`.

### Changed
- Portability fixes found while preparing the Linux build: a function that only an assertion uses is marked as such, and
  the events of the ENet transport are built with all their members. No behavior changes.

## [0.1.0-alpha] - 2026-11-11

### Added
- An optional transport over real sockets: `EnetServerTransport` and `EnetClientTransport`, implementations of
  `ITransport` on ENet 1.3.18 (MIT), built only with `-DTESSERA_BUILD_ENET=ON` (off by default; the option
  downloads ENet, and with it off the build and the 495 tests are unchanged). Sixteen tests in their own executable,
  `tessera_net_enet_tests`, and the `windows-msvc-enet` preset. A section "Over real sockets (ENet)" in the networking
  reference, with an example that is compiled and run as a test, and architecture decision record 0013.
- `bench/head-to-head`: `tessera_h2h_server` and `tessera_h2h_bot` (the Godot baseline scenario on a Tessera Net server
  over ENet) and `run.py`, which runs them, checks that every bot ends with the server's state, and can alternate
  them with the Godot baseline. `tessera_net_bench indicator` prints the time of one `WriteRecord`, the check of how busy
  the machine is.
- Section 8 of the benchmarks page, Tessera Net and Godot's built-in replication in the same scenario (500 to 10,000
  entities, 8 clients, 20 Hz, localhost, median of three alternated rounds): our server kept its 20 Hz tick at every
  size with 4.9 to 7.0 times less CPU and 2.6 to 5.0 times fewer bytes sent to each client, with the caveats on the page
  (the Godot server was saturated at 10,000 entities, the two programs do different work, and the machine had
  background load); and a separate table with the spatial relevancy on.
- `tessera_net_bench network-curve`: the network benchmark at five amounts of change per tick (about 1, 1,000, 2,985,
  10,000 and 33,333 entities moving), with 1 thread and 8 workers, a fitted line as information, and a check that the
  bytes sent are the same with 1 and 8 workers at every point. A chart for it, a subsection of section 7 on the
  benchmarks page, and architecture decision record 0012. The time of the phase grows with the entities
  that move: 1.19 ms on one thread with about one moving, 3.51 ms with 1,000 and 38.08 ms with 33,333, and the curve
  is not a straight line.
- `TESSERA_PROFILE` build option (and the `windows-msvc-profile` preset): the network benchmark prints how long each
  stage of the server's tick takes. The timers do not exist in any other build.
- Pacing and a bandwidth budget for each player: an entity that was sent is sent again only after the player's
  measured round trip (plus a margin), a bucket of bytes with a rate, a peak over 10 seconds and a cap on the size
  of a buffer, and a priority (how long an entity has waited times the weight of its type) for when there is not
  room for everything. In the network benchmark a player gets about 98 kbps on average and 173 kbps at the busiest
  10 seconds (goals: 100 and 200), and 1.09 records per change instead of 3.3. The price is that each entity reaches
  a player less often than it changes when the world changes more than the budget carries. New fields in
  `ReplicationConfig` and `ServerConfig`; the server's defaults are a budget of 100 kbps. See
  `docs/reference/replicated-state.md` and `docs/adr/0011-priority-and-bandwidth-budget.md`.
- Networking: `ITransport` (whole messages on a reliable ordered and an unreliable channel), `SimulatedNetwork`
  (an in-memory network with loss, duplicates, delay and a seed), the protocol (hello from each side, event
  batches, state buffers and acks), `ReplicationServer` (the whole network phase of a tick) and `ClientReplica`
  (applies events in order and state whole or not at all, and confirms it). End-to-end tests with real
  relevancy over a bad network, with 128 players and with random input. See `docs/reference/networking.md`.
- Network benchmark (128 players, 200,000 entities, 5% loss), a chart for it, and a section 7 on the
  benchmarks page. It converges; with the budget above the bytes are within the goal, and the time of the phase
  (about 32 ms per tick on one thread against 3.3 ms) is not yet. Run `tessera_net_bench network` to run it
  alone.
- Architecture decision record 0010 (transport and protocol).
- `ClientReplica::Disconnect()`.
- Replicated state (`ReplicationEngine`, `ReceiverState`): property descriptors for positions, angles, integers,
  bit masks and enums, quantization, and a buffer for each player that holds only what differs from the last
  state the player confirmed. A position update takes about 4.4 bytes. See
  `docs/reference/replicated-state.md`.
- Replicated state benchmark and a chart for it, and a section 6 on the benchmarks page. It does not reach its
  speed goals yet (about 5.8 million updates per second on one thread against 10 million); the numbers and
  what is left are in record 0009.
- Architecture decision record 0009 (replicated state).
- `EntityTable::Config()`.
- Relevancy engine (`RelevancyEngine`): decides which entities each player sees, from the type of entity
  (spatial range, always relevant, owner only), with a hysteresis margin, several observers per player,
  inclusion and exclusion groups, and a short id per entity on each connection. See
  `docs/reference/relevancy.md`.
- `SpatialGrid::ForEachInSphere`, a query that allocates nothing and hands over the stored position.
- Relevancy benchmark (main and smaller scenario), a chart for it, and a section 5 on the benchmarks page.
  It meets its cost goals; the numbers are on the benchmarks page and the design is in record 0008.
- Architecture decision records 0006 (relevancy by entity type), 0007 (the first relevancy version) and 0008
  (incremental relevancy, which replaces 0007).
- `SpatialGrid::ForEachInSphereOfType` and an optional record of the changes made to the grid
  (`SetRecordChanges`, `Changes`, `ClearChanges`). Cells now group their entities by type.
- Benchmarks page with charts (`docs/benchmarks.md`) and a script to regenerate them (`tools/make_charts.py`).
- Source of every benchmark in `bench/`: the data core acceptance benchmark, the comparison with a
  general-purpose ECS, the comparison with Godot nodes, and the baseline of Godot's built-in replication.
- Data core: `EntityId`, paged `EntityTable`, `TypeRegistry`, `ThreadPoolJobSystem`, `CommandBuffers`,
  `SpatialGrid` and `TickLoop` (see `docs/reference/core.md`).
- Integration and determinism tests, and an acceptance benchmark (`tessera_net_bench`).
- Architecture decision records 0004 (per-type paged tables) and 0005 (entity identification).
- Project skeleton: CMake build, presets for Windows and Linux, a minimal test runner, and the
  documentation structure.

### Changed
- The goal for the time of the network phase is now written by the entities that move in a tick: 3.3 ms on one thread
  and 1 ms with eight workers at 1,000 moving entities (a reference load), limits that say not to get worse at 2,985,
  10,000 and 33,333, and 3.3 and 1 ms kept as the long-term goal at 33,333. See record 0012.
- The server marks what changed and applies the players' confirmations on the workers instead of on one thread.
  Both used to take a share of the phase that no number of workers could shrink; with eight workers each now takes
  about 0.5 ms instead of 2.1 and 1.6. Measured before and after in the same session on a busy machine, the phase
  went from 11.8 ms to 8.7 ms with eight workers; with one thread it stays at about 31.5 ms, since there is nothing to
  share. The confirmations of the acks read in a tick are now queued while the transport is read and applied together
  when the reading ends (`ReplicationEngine::QueueConfirm` and `ApplyQueuedConfirms`; `Confirm` still applies at
  once), so the connect and disconnect callbacks do not yet see them. The times of the earlier entries below were
  taken on a quieter machine. The network benchmark's limit for eight workers is now 16 ms.
- `tools/bench_compare.ps1` runs two builds of the network benchmark one after the other and prints the median
  and the range of each, for machines that are busy with other work.
- Relevancy tests the reach of a change while it gathers the changes filed near a player's observers, and sorts
  only those within reach, instead of sorting everything it gathered and checking the distance afterwards. The
  events it produces are the same. In the network benchmark, measured before and after on the same day, the phase
  went from about 42 ms to about 29 ms on one thread and from about 12 ms to about 10 ms with eight workers. The
  main relevancy benchmark, where only 700 entities move, changes little (about 0.8 ms to 0.7 ms on one thread,
  0.24 ms with eight workers).
- The relevancy engine now keeps an anchor for each observer and does a full look around a player only when an
  observer has moved `observer_tolerance` (4 m by default) from it; otherwise it reconsiders only the entities
  that changed nearby. A tick in the main benchmark scenario went from 15.4 ms to 1.03 ms on one thread and from
  2.13 ms to 0.33 ms with eight workers. `RelevancyConfig::recalc_interval_ticks` is removed; new settings are
  `observer_tolerance` and `max_reanchors_per_tick`. The engine takes the grid and the groups as non-constant
  references and must be the only engine on its grid.
- `SpatialGrid` cells now store each entity's position next to its id, so queries no longer look every
  entity up in a hash table. Queries return the same results; a query over 11,600 entities went from about
  1.9 ms to about 0.1 ms (measured with the first version of the relevancy benchmark).
