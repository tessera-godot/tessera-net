# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- `tessera_schema generate --gdscript <output.gd> [--gdscript-class Name]` writes the GDScript of the client from the same
  schema file as the C++: a script with a `class_name` (`GameSchema` unless you choose another), a constant
  `TYPE_<NAME>` for each type, `const HASH: int` (the hash of the file as a signed 64-bit integer, so it can be
  negative) and `static func create() -> TesseraSchema`, which makes the schema with `add_type` and reports a type
  that is refused with `push_error`. One call can write both the header and the script. The class name has to be
  PascalCase and cannot be a name that Godot uses (a list of 1,073 names from Godot 4.7.2 and this extension is built
  into the tool). Numbers are written as the shortest text that gives the same `float`; the output is the same bytes on
  every run, and on Windows and Linux. `add_type` by hand still works. See `docs/reference/schema-file.md`.
- The Godot demo describes its entities with a generated script, `examples/godot-client-demo/example_schema.gd`, made
  from `server/example/example_schema.json` and kept in the repository. A new test in the `ctest` of every preset that
  builds the server, `tessera_schema_demo_check`, makes it again and fails if the kept one is out of date.
- The headless Godot test checks the generated scripts against the extension: for the demo schema and for a test file
  with one property of every kind (generated in the build, with a negative `HASH`), `create().get_hash()` is the `HASH`
  of the script; two calls of `create()` give two schemas; and the `TYPE_` constants are the type numbers the example
  server puts in its entity ids. It has 49 checks (it had 46).
- Tests: `tessera_schema_tests` has 132 tests (it had 104) and the `ctest` of the presets that build the server has one
  more test; the other test programs are unchanged (661, 16, 99, 5 on Windows and 8 on Linux, 79). No message, no
  protocol version (still 3), no hash and nothing in the extension or the server changed.
- A schema file and a tool that makes the server's C++ from it. The entities can be described once, in a JSON file
  (`format_version`, types, properties; the keys and kinds are the ones `TesseraSchema.add_type` takes), and
  `tessera_schema` has three commands: `check` (reads the file, checks it with `ReplicationSchema` itself and prints the
  hash it computes), `hash`, and `generate --cpp <output> [--namespace a::b]`, which writes a header with the column
  structs, the constants, `AddGeneratedTypes`, a function that makes the table of each type with the shard and the type in
  its ids, and the hash of the file as `kSchemaHash`. Errors say the file, the line and the column, and, where there is an obvious one, how to fix them.
  The JSON reader is part of the tool (no new dependency) and reads at most 1 MiB, 16 levels deep. The generated header is
  the same bytes on every run (a test generates it twice and compares it with a copy kept in the repository), and a schema
  that does not check never writes a file. See `docs/reference/schema-file.md` and
  [0020](docs/adr/0020-one-schema-file-and-generated-cpp.md) (Proposed).
- The option `TESSERA_BUILD_SCHEMA_TOOL` (off by default) builds the tool; `TESSERA_BUILD_SERVER=ON` builds it too. The
  default build is unchanged: it still has 661 tests.
- The example server now takes its entities from `server/example/example_schema.json`: the build makes the header in the
  build folder and nothing generated is kept in the source tree, and `example_game.hpp` has no `AddType` of its own. The
  hash of its schema is the same as before (`339718250426401488` with players, `1615724867110258213` for the entity type
  alone), tests pin both numbers and compare the schema with a copy of the one that was written by hand, and no message
  and no behavior changed: the protocol stays at version 3, and the extension and the demo are as they were.
- Tests: `tessera_schema_tests` (104 tests, new) are built and run by every preset that builds the server, and the ctest of
  those presets has two more tests that run the real tool (`tessera_schema_cli_stable`, `tessera_schema_example_check`). `tessera_server_tests`
  has 99 tests (it had 93).
- A format check. `tools/check_format.py` checks, or with `--fix` rewrites, every C++ file that git tracks with
  `clang-format` 18.1.8, the version pinned (with the hashes of its files) in `tools/requirements-format.txt`; it
  refuses any other version. The CI has a `Format` job that runs the same check. `CONTRIBUTING.md` says how to get that
  `clang-format` in a virtual environment, so that nothing is installed on your system.
- Sanitizer builds. The option `TESSERA_SANITIZE` (empty by default, for GCC and Clang) builds everything with the
  sanitizers it names, and the preset `linux-extension-sanitize` uses `address,undefined` and runs the tests that need no
  Godot; the library is not loaded into Godot there. The CI has a `Sanitizers` job with GCC and Clang. ENet's own code is
  built without the two checks that flag how it marks the 32nd fragment of a word (`1 << 31` on a signed integer) and how
  it takes the offset of a header field (through a null pointer); our code and every other check stay on.
- The headless Godot test runs in the CI, in the Linux debug jobs, with the official Godot 4.7.2 downloaded and checked
  against a pinned SHA-512 sum; a step makes the job fail if the test is not registered.
- The headless Godot test counts the warnings of the client: the three prediction rules that fail on purpose must give
  exactly the three expected warnings, and any other `TesseraClient:` warning fails the run.
- `tools/bench_compare.ps1`, which compares the network benchmark of two builds on a machine that is busy, is now
  described in `bench/README.md` and `CONTRIBUTING.md`.
- In the demo project, when the connection drops the demo tries again on its own: after 0.5, 1, 2 and 4 seconds and every
  5 seconds from then on, each wait shifted by up to 20 percent so that clients that lost a server together do not come
  back together. It comes back as a new player, with a new sphere and the prediction set again. A refusal from the server
  is not tried again, and neither is a connection the demo closed itself. The label counts down, for example
  `reconnecting in 3 s (attempt 2)`. The extension and the server did not change, and no message did: the protocol stays
  at version 3.
- Tests for what a client that comes back depends on. The server tests over real sockets (5 on Windows and 8 on Linux, they had 2 and 5) cover
  a client that leaves with notice, one that goes silent (released by ENet after about 6 seconds in loopback on the
  machine where it was measured) and a new client that arrives in between and gets an entity of its own. The extension
  tests (79, they had 77) and the headless Godot test cover a client that drops and connects again several times, with
  the interpolation kept and no prediction until it is set again, and a server that is restarted on the same port under
  a client. The headless Godot test also calls the reconnect timing of the demo. The default build still has 661 tests.
- In the guide to the client in Godot, a section "Reconnecting" with the pattern the demo uses and what a client that
  comes back is. Coming back as the same player does not exist yet.
- Interpolation of the other entities. The core has `Interpolator` and, in `ClientReplica`, `SetInterpolation`,
  `ClearInterpolation`, `AdvanceInterpolation`, `InterpolationSet`, `Interpolating`, `InterpolationSettings`,
  `InterpolationInfo`, `DisplayedTick`, `InterpolationLag`, `InterpolationMemoryBytes`, `InterpolationEntities`,
  `InterpolationStateCount`, `InterpolationStateTick`, `DisplayedWords`, `DisplayedValue` and `StateTickOf`. Each visible
  entity keeps its newest states and the client shows it a few ticks in the past (2 by default, about 66 milliseconds
  at 30 ticks a second): a position is blended between the two states on either side of that moment, an angle goes the
  short way round, and an integer, an enum or a set of flags changes all at once. Nothing is extrapolated: a state that is
  late makes the entity wait. Two neighbouring states more than 8 ticks or more than 2.5 world units apart are not
  blended, so a teleport or a long silence is not slid through. `ReceiverState::Apply` takes an optional
  `IRecordObserver`. It is off unless set, and costs nothing while it is off. No message changed: the protocol stays at
  version 3.
- In the Godot extension, `TesseraClient.set_interpolation(delay_ticks = 2, snap_distance = 2.5)`,
  `clear_interpolation()`, `get_interpolated_property(id, name)`, `is_interpolating()` and `get_interpolation_stats()`.
  `poll()` moves the time shown. For the predicted entity `get_interpolated_property` gives what
  `get_predicted_property` gives. None of these calls is refused inside a prediction rule.
- In the demo project, the other spheres are interpolated, and the label shows the delay.
- A benchmark of what interpolation costs with 1,000 and 10,000 visible entities (`tessera_net_bench_interpolation`; see
  [benchmarks](docs/benchmarks.md#9-interpolation-on-the-client)).
- Tests: the default build now has 659 tests (it had 592), with a counter of memory allocations that shows that taking in
  states and reading values allocate nothing; the server tests (93, they had 83) check interpolation against the example
  server on a simulated network that loses, duplicates and delays messages, and measure the gaps under a budget of
  bytes; the extension tests and the headless Godot test cover the new calls.
- Architecture decision record 0019, "Interpolation of the other entities" (Proposed).
- Prediction and reconciliation of the player's own entity. The core has `Predictor` and, in `ClientReplica`,
  `SetPrediction`, `SetPredictionSettings`, `ClearPrediction`, `AdvancePrediction`, `Prediction`, `PredictionInfo`,
  `PredictionPending`, `PredictedComponents`, `Predicted` and `TickRate`: the game gives a movement rule, `SendInput`
  applies it at once, and each state buffer drops the inputs the server has used, starts from the server's state and
  applies the rest. The difference slides away in 100 milliseconds, or the shown state jumps above 2 world units (both
  can be changed). `ServerStats::input_confirmations_sent` counts the buffers that only confirm an input.
- In the Godot extension, `TesseraClient.set_prediction(entity, properties, rule)`, `clear_prediction()`,
  `get_predicted_property(id, name)`, `is_predicting()`, `get_prediction_stats()` and the properties
  `smoothing_seconds` and `snap_distance`. A rule that fails turns the prediction off with one warning. A rule must not
  call back into the client: while it runs, `send_input`, `poll`, `connect_to`, `disconnect_from_server`,
  `set_prediction` and `clear_prediction` are refused, with the reason in `get_last_error()`.
- In the demo project, the green sphere is predicted with the movement rule of the example server written in GDScript,
  and the label shows the size of the last correction.
- Tests: the default build now has 592 tests (it had 539), with a test of the prediction against the example server on a
  simulated network that loses, duplicates and delays messages; the server tests, the extension tests and the headless
  Godot test cover the prediction too.
- Architecture decision record 0018, "Prediction and reconciliation" (Proposed).
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
- Tests: the default build now has 539 tests (it had 495), with a test of the input against a simulated network that loses
  30 percent of what the client sends; the server tests, the extension tests and the headless Godot test cover the
  example's players and the two new calls.
- Architecture decision record 0017, "Player input" (Accepted).

### Changed
- The Godot demo takes its description of the entities from the generated script `example_schema.gd` instead of two
  `add_type` calls in `main.gd`. `add_type` by hand is unchanged.
- Numbers in the generated C++ (`step` and `range`) are written by the same function as the ones in the GDScript, which
  gives the shortest text for the same `float` the same way in every standard library; for the values that are used
  (such as `0.01` and `2048.0`) the text did not change, and the sample header kept in the tests is the same.
- The C++ code was formatted with `clang-format` (the `.clang-format` did not change), in one commit for each of
  `core/include`, `core/src`, `server`, `extension`, `tests` and `bench`: 79 of the 174 files. Nothing else changed in
  those commits: the test counts are the same as before (661, 16, 93, 8 on Linux and 5 on Windows, 79; no message
  format changed), and the compiled objects of the core, the server and the extension are byte for byte the same on
  Linux with GCC. The tests, and one benchmark file, differ only where a line number is printed or the order of the
  `#include` lines moved.
- The roadmap says that records 0018 and 0019 are accepted, and `tools/build.sh` names the right number of tests.
- **The protocol version is now 3** (0.2.0-alpha spoke version 1). Clients and servers of 0.2.0-alpha no longer connect to
  the new ones: the server refuses their hello with `VersionMismatch`. Version 2 added the `Input` message; version 3
  adds a field to the header of the `StateBuffer`, the newest input of the player that the state reflects. No other
  message changed.
- The server sends a buffer with only the header to a player that is sending input while nothing about its entities
  changes, so that the input is confirmed. While the bandwidth budget leaves entities out of a player's buffer, the
  buffer keeps the input number it sent before, because its state is behind the inputs the server has read.

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
