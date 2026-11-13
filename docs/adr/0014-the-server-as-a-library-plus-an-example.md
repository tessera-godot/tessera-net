# 0014. The server as a library plus an example

- **Status:** Accepted
- **Date:** 2026-11-11

## Context
[Record 0002](0002-standalone-dedicated-server.md) says the dedicated server is a separate program with no Godot in
it, and that the game's logic is written in C++. Until now that program did not exist: `server/` held a README. The
core, the replication layer and the ENet transport ([0013](0013-enet-as-the-first-real-transport.md)) were there, but
nothing started a server that someone could run, stop and read the log of, and the code had only ever been built with
MSVC on Windows.

The game's code has to be in the server, so the server cannot be a finished program. What it can be is everything
around that code: reading the settings, building the world the game describes, running the tick loop at a fixed rate,
stopping cleanly and writing a log. We chose to ship that as a library, with a small program next to it that shows how
to use it. A plugin system that loads the game's code from a shared library was left out on purpose.

## Decision
- **`tessera_server`, a static library in `server/`.** It has `ServerConfig` (the settings), `IGameLogic` (what the
  game implements), `ServerHost` (the loop) and a logger with an optional summary line every few seconds. It links only
  the core and includes no ENet header: the host is given an `ITransport` by whoever builds it. It is built with
  `-DTESSERA_BUILD_SERVER=ON` and is off by default, so the default build and its 495 tests do not change.
- **`tessera_example_server`, a small program that is the model.** It reads the configuration, opens an ENet transport,
  builds a `ServerHost` around a small game (entities that walk and bounce) and runs it. A game writes its own `main`
  with the same shape, and the example's is short.
- **No dynamic plugin.** An executable that loads the game from a DLL or a shared object would have to settle the C++
  binary interface across that boundary (the compiler, the standard library and the flags have to match, and the
  standard types cannot be passed freely), the loading and unloading of code, which differs between Windows and Linux,
  and debugging a host and a library as two things. Nothing we have measured needs it. When the Godot extension
  arrives it will need its own answer, and this record is the place to reopen.
- **`IGameLogic` has six members**, the first version of the interface, and it may change. `DescribeWorld` returns the
  schema, the relevancy settings and the grid cell size (the replication layer needs them when it is built, which is why
  it is a member and not a call made later). `OnStart` receives a context with the shard id, the tick rate, the
  replication server, the grid, the groups, the jobs, the logger and a function that asks for a stop. `OnTick` runs the
  simulation and ends by handing each table to `Refresh`; the host then runs the network phase and flushes the
  transport. `OnPlayerConnected` and `OnPlayerDisconnected` come while the transport is read. `OnShutdown` comes once.
  The callbacks must not throw: an exception that leaves `OnTick` is logged, `OnShutdown` still runs, and `Run` returns
  1. The same goes for an exception from `OnStart`, from the final flush or from `OnShutdown`: `Run` logs it and
  returns 1, and a host whose `OnStart` threw never ticks. `OnTick` runs once per tick, and the tick rate is a target:
  after a tick that overran there is no catch-up, so ticks are not exactly `1 / tick_rate` seconds apart.
- **Stopping is an atomic flag.** `ServerHost::RequestStop()` stores to a lock-free atomic, with no lock and no
  allocation, so it can be called from a signal handler, from another thread or from inside a callback. The host
  finishes the tick in progress, hands what is pending to the transport, calls `OnShutdown` once and `Run` returns 0. A
  second request changes nothing, and a request made before `Run` starts means no tick at all. The code that turns
  signals into a request lives in the example, not in the library: Ctrl+C, Ctrl+Break and closing the console on
  Windows, and SIGINT and SIGTERM on Linux. The handler only sets a flag, and a thread of the example passes it on to
  the host. It is the only global state in the server, and it is not in the library.
  `ITransport` cannot refuse new connections, so after a stop nobody reads it any more, and whoever owns the transport
  closes it.
- **Settings come from a file and from arguments.** A file of `key=value` lines and `--key=value` arguments, with the
  arguments on top. A key that is unknown or repeated in one source, a value that is not a plain number (or a log level)
  and a value out of range are errors that name the key, and the server does not start. There are seven keys: `port`,
  `max_players`, `tick_rate`, `threads`, `shard_id`, `log_level` and `summary_interval`.
- **`shard_id` is the only hook for splitting the world.** It is a setting (0 to 4,095), the host passes it to the game
  in the context, and the game puts it in `TableConfig{shard, type}`, which is how it reaches the 12 bits of the entity
  id ([0005](0005-entity-identification.md)). The library has no global state and no singleton, and two hosts can live in
  one process (a test does that), so nothing in it stands in the way of a process that runs several shards.

## Consequences
- **What was not done.** Moving entities from one server to another, borders between shards, a protocol between
  shards, authentication, encryption, reconnecting into an old session, a plugin loader and measurements on Linux. The
  shard id is a number in the ids and nothing more.
- **The game writes its `main`.** That is the price of not having a plugin: a little code per game, and a rebuild to
  change the logic. In return the whole server is one program that a debugger and a profiler understand.
- **The interface will change.** Six members is what the first example needed. The Godot extension, prediction and
  input are likely to add to it, and each change comes with its own record.
- **Linux and the CI.** A workflow in the repository builds and tests the code on Ubuntu with GCC and with Clang and on
  Windows with MSVC, in Release and Debug, with and without ENet, with warnings as errors, and it also builds the
  default configuration. When this record was written the workflow had not run. It was checked locally on Windows with MSVC, and in an
  Ubuntu 24.04 container (GCC 13 and Clang 18, Release and Debug, with warnings as errors); the workflow has passed on
  GitHub since.
- **More to maintain.** A build option, two test executables (the server library, and the example over ENet in
  loopback) and a script that starts the example as a process and sends it signals.
