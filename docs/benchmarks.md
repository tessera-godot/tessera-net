# Benchmarks

Numbers, how they were measured, and how to reproduce them. Every benchmark has its source in
[`bench/`](../bench/README.md). All results come from one desktop machine (AMD Ryzen 9 5900X, 12 cores,
64 GB RAM, Windows 11) that was also running other programs, so results vary from run to run. We report the
median, and the range where the variation is large.

## 1. Where Godot's built-in replication stops scaling

This is the measurement that motivates the project. Source: [`bench/godot-replication`](../bench/godot-replication/).

![Frame rate of a Godot server using the built-in replication as the entity count grows](images/baseline-godot-replication.svg)

| Entities | Server fps (target 60) | Server CPU (1 core = 100%) | Bandwidth per client |
|---|---|---|---|
| 500 | 60 | 10.5% | 189 KB/s |
| 2,000 | 59.4 | 36.6% | 751 KB/s |
| 5,000 | 26.8 | 89.2% | 1.7 MB/s |
| 10,000 | 7.4 | 97.7% | 1.9 MB/s |

A headless Godot 4.7.2 server; each entity is a node with a `MultiplayerSynchronizer` replicating one
position at 20 Hz; 8 bot clients see every entity; everything runs over localhost. That works out to about
19 bytes per position update and about 0.85 million entity-client updates per second per core: the cost
grows with entities times clients, on a single thread.

Limits of this test: no network latency or packet loss, only positions, one run per scenario, no
visibility filtering. Treat it as an order of magnitude.

## 2. One million entities: Godot nodes versus the entity table

If every entity is a node, creating, updating and removing a million of them is expensive. The entity table
does the same job on plain arrays. Source: [`bench/godot-nodes`](../bench/godot-nodes/).

![Time to create, destroy and update one million entities as Godot nodes and in the entity table](images/entities-vs-godot-nodes.svg)

| Operation (1,000,000 entities) | Godot `Node3D` objects | Entity table | Ratio |
|---|---|---|---|
| Create | 615 ms | 12.4 ms | 50x |
| Destroy | 557 ms | 6.2 ms | 90x |
| Update every position once | 168 ms | 1.0 ms | 168x |

Three executions of the benchmark, each the median of 5 runs, gave 584 to 622 ms, 532 to 569 ms and
164 to 170 ms for the nodes, and 12.4 to 16.0 ms, 5.9 to 6.6 ms and 0.99 to 1.03 ms for the table.

How to read this fairly:
- The nodes are created, read and written from a GDExtension through the public API, without a scene tree.
  Code inside the engine (a C++ module) skips the API call overhead and would be faster; GDScript would be
  slower. The table runs in the same process with the same arithmetic.
- This compares how entities are **stored**, not how they are replicated. The core does not replicate yet, so
  there is no replication comparison to show; when there is, it will be added here.
- A node does more than a table slot does (transforms, signals, scene tree). That is exactly why it is
  heavy as the unit of a huge simulation, and also why it remains the right tool for what the player sees.

## 3. The entity table versus a general-purpose ECS

An entity-component-system library lets you add and remove components at any time. The entity table cannot,
and in return it is faster at the operations a replicated server repeats every tick. Source:
[`bench/ecs-comparison`](../bench/ecs-comparison/) (compares against EnTT 3.16).

| Operation (1,000,000 entities, one thread) | Entity table | EnTT |
|---|---|---|
| Create | 14.0 ms (11.5 to 20.7) | 102 ms (85 to 162) |
| Destroy | 13.7 ms (12.5 to 15.5) | 201 ms (191 to 253) |
| Churn: replace 10%, ten rounds | 80 ms (32 to 157) | 1,784 ms (1,446 to 2,876) |
| Update every position once | 1.6 ms (1.4 to 2.8) | 3.4 ms (2.9 to 5.2) |
| Write the 10% that changed (dirty flag) | 0.79 ms (0.73 to 1.19) | 5.2 ms (4.2 to 7.8) |
| Write the 10% that changed (tag component) | not needed | 19 ms (15 to 35) |

Median of five executions of the program, with the range in parentheses. The comparison favours the table
because it does less: it cannot add components to a live entity or answer arbitrary queries. It shows what
that generality costs for this use, not that EnTT is slow. EnTT groups and memory use were not measured.

One thing we have not explained: destroying a million entities takes about 6 ms in the data core benchmark
(section 4) but about 13 ms in this program, consistently. Both programs use the same table code, so compare
numbers only within one program.

## 4. The data core

![Measured value versus limit for each data core criterion](images/data-core-acceptance.svg)

| Criterion | Limit | Measured (median of 3 runs) |
|---|---|---|
| Create 1 M entities | 32 ms | 11.8 ms |
| Destroy 1 M entities (in creation order) | 12 ms | 5.7 ms |
| Destroy 1 M entities (random order) | no limit | about 10 to 11 ms |
| Churn: destroy and recreate 10%, ten times | 420 ms | 36.7 ms |
| Iterate 1 M entities, one pass | 3 ms | 1.4 ms |
| Bookkeeping per entity | 8 bytes | 4.25 bytes |
| Parallel efficiency, 8 workers | at least 70% | 91 to 95% |
| Tick jitter at 30 Hz, p99, no load | 2 ms | 0.06 ms |
| Same result with 1 and 8 workers, 1,000 ticks | identical | identical |

The limits were chosen before the final measurements, with a safety margin. They work as a regression
guard: the benchmark fails if the core becomes slower than its limit.

Conditions: 1,000,000 entities, Release build (MSVC), median of 5 runs inside each execution. The parallel
efficiency test is pure CPU work (about 100 ns per item), so real workloads that touch more memory will
scale less well. Only Windows has been measured so far; Linux results will be added.

## 5. Relevancy

How long does it take to decide what each player needs to know about? Source:
[`bench/bench_relevancy.cpp`](../bench/bench_relevancy.cpp). How the engine works is in the
[relevancy reference](reference/relevancy.md), and the decisions behind it are in records
[0006](adr/0006-relevancy-by-type-and-groups.md) and [0008](adr/0008-incremental-relevancy.md).

![Relevancy time per tick: measured value, regression limit and goal](images/relevancy-update.svg)

The main scenario: 128 players and 1,000,000 entities spread evenly over 10 km by 10 km (10,000 per square
kilometre), grid cells of 128 m, 700 entities moving every tick by 0.2 m. Every player has one observer that
moves at 5 m/s, and one player in ten at 25 m/s (a vehicle). Entities belong to four types that can be seen
from 100, 250, 400 and 600 m (60%, 25%, 10% and 5% of the entities), with an 8 m hysteresis margin. The
observer tolerance is 4 m and at most 32 full looks are done per tick. About 6.25 players per tick need a
full look around them.

| Measure | Result (median of 4 executions, range) | Limit | Goal |
|---|---|---|---|
| Entities visible to a player | 1,787 | between 1,000 and 2,000 | n/a |
| Time per tick, 1 thread | 1.03 ms (0.97 to 1.44) | 2.0 ms | 1.5 ms |
| Time per tick, 8 workers | 0.33 ms (0.31 to 0.40) | 0.7 ms | 0.4 ms |
| Memory per player | about 0.16 MB | 2 MB | n/a |
| Same events with 1 and 8 workers | identical | identical | n/a |

Each execution of the program reports the median of 5 runs of 30 ticks, after 9 warm-up ticks. We executed the
program four times and give the median of those four values and their range. The four values, in
milliseconds per tick:

| Execution | Main, 1 thread | Main, 8 workers | Smaller, 1 thread | Smaller, 8 workers |
|---|---|---|---|---|
| 1 | 0.97 | 0.31 | 0.27 | 0.09 |
| 2 | 1.08 | 0.34 | 0.34 | 0.11 |
| 3 | 1.44 | 0.40 | 0.29 | 0.10 |
| 4 | 0.97 | 0.31 | 0.29 | 0.09 |

The same program also measures, for information only (medians of the four executions, in milliseconds per
tick):

| Case | 1 thread | 8 workers |
|---|---|---|
| Smaller scenario: 32 players, 250,000 entities at the same density, 175 moving per tick | 0.29 | 0.10 |
| Half of the players moving at 25 m/s | 2.46 | 0.57 |
| Stress: every observer moves up to 10 m and every moving entity up to 30 m per axis each tick | 4.68 | 0.83 |

A separate measurement of the grid query by type, the sort and the merge for one player takes about 78 µs; a full look in the engine does more than that (groups, always-relevant and owned entities, ids), so treat it as a lower bound.

How to read this:
- **The limits are regression guards, not goals.** They are about twice the median, so the run-to-run
  variation does not fail the benchmark and a real slowdown does. The goals (1.5 ms and 0.4 ms) are what the
  engine was built to reach. One 8-worker execution measured 0.40 ms, exactly the goal, which is why the
  guard cannot sit at the goal. We allow 3.3 ms on one thread for the whole network phase of a 128-player
  server ([README](../README.md#design-goals)); relevancy takes about a third of it on one thread and about a
  tenth with eight workers.
- **Where it started.** The first version rebuilt each player's visible set every few ticks and measured 15.4
  ms on one thread and 2.13 ms with eight workers under this same movement (136 to 147 ms and 23 to 24 ms
  before the grid stored positions inside its cells; that version of the program is in the git history).
  Asking the grid per entity type, keeping the visible set sorted, reconsidering only the entities that
  changed and finding those through an index by grid cell made it 15 times faster on one thread and 6.5
  times with eight workers. See [record 0008](adr/0008-incremental-relevancy.md).
- **Fast players cost more.** A player moving at 25 m/s needs a full look every 5 ticks with a 4 m tolerance.
  With half of the players doing that, a tick takes about 2.4 times as long on one thread and 1.7 times with eight workers. If your game has many fast
  players, use more workers or a larger tolerance.
- **Scaling.** The cost follows the number of players, how many entities move near them and how fast the
  players move, not the size of the world. The smaller scenario has a quarter of the players and costs about
  3.5 times less on one thread.
- **Variation.** The machine was running other work during the measurements, and one execution was clearly
  slower than the rest (1.44 ms and 0.40 ms).

Conditions: Release build (MSVC), the same desktop machine as above, Windows only so far. The benchmark uses
one observer per player and no exclusion or inclusion groups. Memory is the growth of the process's resident
memory after adding the players and running a few ticks, divided by the number of players; memory the
process had already freed can hide part of the growth, so treat it as a lower bound.

## 6. Replicated state

How fast can the server turn entity state into the bytes each player needs, and how large are they? Source:
[`bench/bench_replication.cpp`](../bench/bench_replication.cpp). How it works is in the
[replicated state reference](reference/replicated-state.md) and the decisions behind it are in
[record 0009](adr/0009-replicated-state.md). There is no network and no live relevancy here: every player is given
a fixed set of visible entities, so the numbers are those of replicated state alone.

![Replicated state: time to write the buffers of one tick, measured value, regression limit and goal](images/replication-update.svg)

The scenario: 128 players, each seeing 1,500 of 200,000 entities, whose state is a position, an angle, a health,
16 flags and a state number. Players confirm the buffers of a tick two ticks later. In the first case every visible
entity changes on every tick, so each tick writes 192,000 records. In the typical case each entity changes every
sixth tick (five times a second at 30 Hz).

| Measure | Result (median of 4 executions, range) | Limit | Goal |
|---|---|---|---|
| Updates written per second, 1 thread | 6.5 million (6.4 to 6.6) | 3.2 million | 10 million |
| Efficiency with 8 workers | 46% (41% to 46%) | 22% | 70% |
| Bytes per position update | 4.4 | 8 | 8 |
| Memory per entity (stored state) | 54 B | 128 B | 128 B |
| Memory per player | 0.64 MB | 4 MB | 4 MB |
| Bytes per tick for a player who sees nothing that changed | 1 | 1 | 1 |
| Same bytes with 1 and 8 workers | identical | identical | identical |

**The first two goals are not met.** The limits are regression guards, at about half of what the engine measures
today, so the benchmark fails if it gets slower; the goals are what the next version is for.

The four values of the first two rows and the main times, per tick (milliseconds, unless marked):

| Execution | Updates per second, 1 thread | `Update`, 1 thread | `Update`, 8 workers | Efficiency | Typical case, 1 thread |
|---|---|---|---|---|---|
| 1 | 6.49 million | 29.6 | 8.1 | 46% | 22.7 |
| 2 | 6.62 million | 29.0 | 7.9 | 46% | 23.2 |
| 3 | 6.44 million | 29.8 | 9.1 | 41% | 22.4 |
| 4 | 6.51 million | 29.5 | 8.3 | 44% | 21.9 |

Other numbers from the same runs (medians, in milliseconds per tick):

| Case | Time |
|---|---|
| Quantizing the 200,000 entities that changed | 8.8 |
| Confirmations of one tick, all 128 players | 5.7 |
| Typical case: `Update` and confirmations, 1 thread | 22.7, for 96,000 records |
| Typical case: quantizing the entities that changed | 2.0 |
| Writing one position record on its own, data in the processor cache | 0.000033 (33 nanoseconds) |

How to read this:
- **Where the time goes.** With one player and every entity changing, the engine spends 3.1 ms to write 1,500
  records, and with nothing changing it spends nothing. Before it kept an index of who sees what, it spent that
  time looking each changed entity up in every player's visible set: 25.6 million searches a tick, 328 ms in all.
  With the index the same tick takes 29.6 ms (about 11 times less) and the throughput went from 0.6 to 6.5
  million a second. The rest is about 5 ms to mark who needs what, on one thread, and about 128 nanoseconds per
  record to write it, against 47 for writing it alone with the data in cache.
- **What did not help.** Fetching the state of the entities ahead of use into the cache made no difference
  (6.4 to 6.8 million a second with it and 6.4 to 6.6 without). We think the per-player state, about 80 MB for 128
  players, is what does not fit in the cache, but we have not shown it with a profiler.
- **The typical case is three times larger than it needs to be.** A changed entity is sent on three ticks in a
  row until the confirmation of the first send comes back, so a tick writes 96,000 records where 32,000 carry
  new information. How often to repeat belongs to the layer that decides what goes first, not to this one.
- **Size.** The 4.4 bytes are the whole buffer divided by its records, so the 1-byte end marker of each buffer is
  included. Position records alone take 4.4 bytes when written one by one; 3.9 bytes in the typical case, where
  the entity moves less between confirmed states.
- **Memory.** The 54 bytes per entity are the stored state of the entity plus some room for the list of changes.
  The 0.64 MB per player includes an equal part of the index of who sees what.

Conditions: Release build (MSVC), the same desktop machine as above, Windows only so far, one observer per player
and no exclusion or inclusion groups. The machine was running other programs, and the variation between executions
is large (the efficiency with eight workers ranged from 41% to 96% before the index was added).

## Reproduce

Windows, from the repository root (details and prerequisites for each benchmark are in
[`bench/README.md`](../bench/README.md)):

```powershell
# data core (section 4), relevancy (section 5) and replicated state (section 6): one program, a few minutes on the
# machine above
cmake --preset windows-msvc -DTESSERA_BUILD_BENCH=ON
cmake --build --preset windows-msvc-release --target tessera_net_bench
./build/windows-msvc/bench/Release/tessera_net_bench.exe

# entity table vs EnTT (section 3); downloads EnTT the first time
cmake --preset windows-msvc -DTESSERA_BUILD_ECS_COMPARISON=ON
cmake --build --preset windows-msvc-release --target tessera_net_ecs_comparison
./build/windows-msvc/bench/ecs-comparison/Release/tessera_net_ecs_comparison.exe
```

The charts are plain SVG files generated by `python tools/make_charts.py` (no dependencies). The numbers are
at the top of that script.
