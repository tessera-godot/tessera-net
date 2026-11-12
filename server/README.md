# server

The dedicated server, as a library plus a small example program. No dependency on the Godot engine. The reasons for
this shape are in [record 0014](../docs/adr/0014-the-server-as-a-library-plus-an-example.md), and the full guide is the
section "Running a server" of the [networking reference](../docs/reference/networking.md#running-a-server).

- `tessera_server` (this folder, `include/` and `src/`): the settings (`ServerConfig`), the interface your game
  implements (`IGameLogic`), the host that runs the tick loop and stops cleanly (`ServerHost`), and a logger with an
  optional summary line.
- `tessera_example_server` (`example/`): a small game, entities that walk and bounce, on a real UDP port over ENet. It is
  the model for your own `main`.

## Building

Both are off by default. The library needs only the core:

```powershell
cmake --preset windows-msvc-server
cmake --build --preset windows-msvc-server-release
```

The example program also needs ENet, which CMake downloads on the first configure:

```powershell
cmake --preset windows-msvc-enet
cmake --build --preset windows-msvc-enet-release --target tessera_example_server
```

On Linux the presets are `linux-server-release`, `linux-server-debug`, `linux-enet-release` and `linux-enet-debug`
(CMake 3.21+, Ninja, GCC 11+ or Clang 14+ with libstdc++). It was built and tested locally on Windows with MSVC and in
an Ubuntu 24.04 container with GCC 13 and Clang 18. A CI workflow for Ubuntu and Windows is in the repository but has
not run yet; Linux is claimed once it shows it.

## Running the example

```powershell
./build/windows-msvc-enet/server/Release/tessera_example_server.exe --port=7777 --tick_rate=30
./build/windows-msvc-enet/server/Release/tessera_example_server.exe --config=server.cfg --log_level=debug
```

Stop it with Ctrl+C (SIGINT or SIGTERM on Linux). It finishes the tick in progress, hands what is pending to the network
and exits with 0. A bad setting exits with 2 and a message that names the key; a port that is in use exits with 1.
`--help` lists the settings.

## Settings

A file of `key=value` lines (`#` starts a comment) named with `--config=<path>`, and `--key=value` arguments, which win
over the file. A key that is unknown or given twice in one source, a value that is not a plain number and a value out of
range stop the server before it starts.

| Key | Meaning | Range | Default |
|---|---|---|---|
| `port` | UDP port to listen on | 1 to 65,535 | 7777 |
| `max_players` | Players at once (the example takes at most 4,095, ENet's limit) | 1 to 65,535 | 128 |
| `tick_rate` | Ticks per second | 1 to 1,000 | 30 |
| `threads` | Threads for parallel work, the main one included | 1 to 256 | 1 |
| `shard_id` | Number written into every entity id | 0 to 4,095 | 0 |
| `log_level` | `debug`, `info`, `warn` or `error` | | `info` |
| `summary_interval` | Seconds between summary lines (tick, players, message bytes per second, as `ITransport::Stats()` counts them, not ENet wire bytes); 0 turns them off | 0 to 3,600 | 0 |

## Writing your own server

Copy `example/` and change three things. Write a class that implements `IGameLogic` (`DescribeWorld` says which
properties are replicated and who sees what, `OnStart` makes the tables and entities, `OnTick` simulates and hands the
changes to `Refresh`). Put `context.shard_id` in the `TableConfig` of each of your tables. And keep the shape of `main`:
`ResolveConfig`, a transport, a `ServerHost` around your game, `Run()`. Callbacks must not throw.
