# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Relevancy engine (`RelevancyEngine`): decides which entities each player sees, from the type of entity
  (spatial range, always relevant, owner only), with a hysteresis margin, several observers per player,
  inclusion and exclusion groups, and a short id per entity on each connection. See
  `docs/reference/relevancy.md`.
- `SpatialGrid::ForEachInSphere`, a query that allocates nothing and hands over the stored position.
- Relevancy benchmark (main and smaller scenario), a chart for it, and a section 5 on the benchmarks page.
  The first version does not reach its cost goals yet; the numbers and the way forward are in record 0007.
- Architecture decision records 0006 (relevancy by entity type) and 0007 (the first relevancy version).
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
- `SpatialGrid` cells now store each entity's position next to its id, so queries no longer look every
  entity up in a hash table. Queries return the same results; a query over 11,600 entities went from about
  1.9 ms to about 0.1 ms (measured with the first version of the relevancy benchmark).
