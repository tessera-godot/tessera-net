# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Replicated state (`ReplicationEngine`, `ReceiverState`): property descriptors for positions, angles, integers,
  bit masks and enums, quantization, and a buffer for each player that holds only what differs from the last
  state the player confirmed. A position update takes about 4.4 bytes. See
  `docs/reference/replicated-state.md`.
- Replicated state benchmark and a chart for it, and a section 6 on the benchmarks page. It does not reach its
  speed goals yet (about 6.5 million updates per second on one thread against 10 million); the numbers and
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
