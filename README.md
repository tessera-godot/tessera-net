# Tessera Net

High-performance, dedicated-server networking for Godot 4, built for large open worlds.

Part of **Tessera**, a family of open-source tools for building large multiplayer games in Godot.

> **Status: pre-alpha.** The engine-independent data core (entities, fixed-step tick loop, spatial grid,
> job system) and a first relevancy engine (deciding what each player sees) are implemented and tested.
> Sending entity state, the transport, the Godot plugin and the dedicated server are not built yet, so
> nothing here is ready for production. Follow the [roadmap](docs/roadmap.md) for progress.

## Why this exists

Godot's built-in multiplayer is great for small games. It replicates every visible node to every client,
which stops scaling at a few thousand entities. In our baseline measurement a single server core handled
roughly 0.85 million entity updates per second at about 19 bytes each, and degraded sharply above
2,000 entities with 8 clients. Open-world survival games need far more than that.

This project builds the missing pieces so Godot developers can make those games:

- **Interest management**: each player only receives what is near them.
- **Dormancy**: entities that are not changing cost nothing.
- **Delta compression and quantization**: small packets, small bills.
- **Multi-threaded replication**: spreads the work across CPU cores.
- **A standalone dedicated server**: a lean binary that does not need the Godot engine.
- **Prediction and reconciliation** for smooth movement under lag.
- **Editor tools**: configure everything from the Godot editor, no networking PhD required.

![Frame rate of a Godot server using the built-in replication as the entity count grows](docs/images/baseline-godot-replication.svg)

Part of the problem is how entities are stored. A million entities kept as nodes are slow to create, update
and remove, while the entity table in this project does the same work on plain arrays:

![Time to create, destroy and update one million entities as Godot nodes and in the entity table](docs/images/entities-vs-godot-nodes.svg)

Read the full [motivation](docs/motivation.md) and the [benchmarks](docs/benchmarks.md).

## Design goals

| Goal | Target |
|---|---|
| Replication throughput | at least 10 million entity updates per second per core |
| Network phase per tick, 128 clients | 3.3 ms or less on one thread |
| Position update size | 8 bytes or less |
| Idle (dormant) entity | zero CPU and zero bytes per tick |
| Registered entities | 1 million, cost proportional to changes |

These are targets, not results. Measured numbers are published as they exist, including the ones that miss
their target: the first relevancy engine takes about 14 ms per tick for 128 players among a million entities
(one thread), and its goal is 1.5 ms. See the [benchmarks](docs/benchmarks.md#5-relevancy).

## What exists today

The **data core** (`core/`): entity storage in paged tables, 64-bit entity ids, a fixed-step tick loop with
five phases, a sparse 3D spatial grid, a thread-pool job system and deterministic command buffers. See the
[core reference](docs/reference/core.md) for how each part works and a runnable example.

![Measured value versus limit for each data core criterion](docs/images/data-core-acceptance.svg)

The **relevancy engine** (`core/`): for every player, which entities do they need to know about? Each type of
entity gets a range (or is always visible, or visible only to its owner), groups make exceptions, and every
visible entity gets a short id on that player's connection. It runs on many threads and gives the same result
with one worker or many. See the [relevancy reference](docs/reference/relevancy.md).

![Relevancy time per tick: measured value, regression limit and goal](docs/images/relevancy-update.svg)

The relevancy goals are not met yet; the chart shows where it stands and what the next version is for.

All numbers, conditions and caveats are in [docs/benchmarks.md](docs/benchmarks.md).

## Quick start

Not available yet. When the first release is ready you will download a ZIP, copy it into your project and
enable the plugin. See [Getting started](docs/getting-started.md) for the current status.

## Building from source

You only need this if you want to contribute or try unreleased code.

Windows (Visual Studio 2022 with the C++ workload, CMake 3.21+):

```powershell
./tools/build.ps1
```

Linux (CMake 3.21+, Ninja, GCC 11+ or Clang 14+):

```bash
./tools/build.sh
```

To also build and run the acceptance benchmark (Release builds only):

```powershell
cmake --preset windows-msvc -DTESSERA_BUILD_BENCH=ON
cmake --build --preset windows-msvc-release --target tessera_net_bench
./build/windows-msvc/bench/Release/tessera_net_bench.exe
```

## Repository layout

| Folder | Contents |
|---|---|
| `core/` | The engine-independent C++ library (the heart of the project) |
| `extension/` | The Godot GDExtension that exposes the core to the editor and GDScript |
| `server/` | The standalone dedicated server program |
| `addons/` | The Godot plugin shipped to users |
| `examples/` | Small projects that work as soon as you open them |
| `tests/` | Automated tests |
| `bench/` | Benchmarks: the data core, the relevancy engine and the comparisons shown in the docs |
| `docs/` | Documentation, architecture and decision records |
| `tools/` | Build and packaging scripts |

## Documentation

- [Documentation home](docs/index.md)
- [Getting started](docs/getting-started.md)
- [Concepts explained simply](docs/concepts.md)
- [Core library reference](docs/reference/core.md)
- [Relevancy reference](docs/reference/relevancy.md)
- [Benchmarks](docs/benchmarks.md)
- [Architecture](docs/architecture.md)
- [Roadmap](docs/roadmap.md)
- [Architecture decision records](docs/adr/README.md)

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first.

## License

- **Code:** [Apache License 2.0](LICENSE). Keep the copyright and the [NOTICE](NOTICE) when you redistribute.
- **Documentation:** [CC BY 4.0](docs/LICENSE.md).
