# Benchmarks

Results, charts and how to read them are on the [benchmarks page](../docs/benchmarks.md). This folder holds
the programs that produce them.

| Folder | What it measures | Needs |
|---|---|---|
| `bench_acceptance.cpp`, `bench_relevancy.cpp`, `bench_replication.cpp`, `bench_network.cpp` | The data core, the relevancy engine, replicated state and the whole network path against their limits | CMake and a C++20 compiler |
| [`ecs-comparison/`](ecs-comparison/) | The entity table against a general-purpose ECS (EnTT) | CMake, C++20; downloads EnTT on first configure |
| [`godot-nodes/`](godot-nodes/) | One million Godot nodes against the entity table | Godot 4.5 or newer, godot-cpp, SCons |
| [`godot-replication/`](godot-replication/) | Godot's built-in replication as entities grow | Godot 4.x, Python with `psutil` |

## Data core and ECS comparison

```powershell
cmake --preset windows-msvc -DTESSERA_BUILD_BENCH=ON -DTESSERA_BUILD_ECS_COMPARISON=ON
cmake --build --preset windows-msvc-release
./build/windows-msvc/bench/Release/tessera_net_bench.exe
./build/windows-msvc/bench/ecs-comparison/Release/tessera_net_ecs_comparison.exe
```

Run benchmarks only in a Release build, and close other heavy programs first: the numbers vary when the
machine is busy. The network benchmark takes the longest; run it alone by adding the word `network` after the
name of the program. The word `network-curve` runs the network path at five amounts of change per tick (the
first moves about one entity, to measure the fixed cost) and fits a line to the time of the phase; it takes a few
minutes and is not part of the full run (add `reverse` to play the points from the busiest to the quietest).

## Godot nodes

Build the extension with SCons against your godot-cpp and the API file of the Godot you will run it with, then
run the script headless:

```
godot --headless --dump-extension-api            # creates extension_api.json
cd bench/godot-nodes
scons platform=windows target=template_release godot_cpp_dir=<path to godot-cpp> custom_api_file=<path to extension_api.json>
godot --headless --path project -s run.gd -- 1000000 5
```

The script prints one JSON line with the median time of each operation, in milliseconds.

## Godot replication

See [`godot-replication/README.md`](godot-replication/README.md).
