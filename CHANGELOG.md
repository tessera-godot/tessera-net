# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Pacing and a bandwidth budget for each player: an entity that was sent is sent again only after the player's
  measured round trip (plus a margin), a bucket of bytes with a rate, a peak over 10 seconds and a cap on the size
  of a buffer, and a priority (how long an entity has waited times the weight of its type) for when there is not
  room for everything. In the network benchmark a player gets about 98 kbps on average and 172 kbps at the busiest
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
  (about 50 ms per tick on one thread against 3.3 ms) is not yet. Run `tessera_net_bench network` to run it
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
- The relevancy engine now keeps an anchor for each observer and does a full look around a player only when an
  observer has moved `observer_tolerance` (4 m by default) from it; otherwise it reconsiders only the entities
  that changed nearby. A tick in the main benchmark scenario went from 15.4 ms to 1.03 ms on one thread and from
  2.13 ms to 0.33 ms with eight workers. `RelevancyConfig::recalc_interval_ticks` is removed; new settings are
  `observer_tolerance` and `max_reanchors_per_tick`. The engine takes the grid and the groups as non-constant
  references and must be the only engine on its grid.
- `SpatialGrid` cells now store each entity's position next to its id, so queries no longer look every
  entity up in a hash table. Queries return the same results; a query over 11,600 entities went from about
  1.9 ms to about 0.1 ms (measured with the first version of the relevancy benchmark).
