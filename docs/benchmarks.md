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

| Measure | Result (median of 6 executions, range) | Limit | Goal |
|---|---|---|---|
| Updates written per second, 1 thread | 5.8 million (5.4 to 6.0) | 2.9 million | 10 million |
| Efficiency with 8 workers | 44% (40% to 48%) | 22% | 70% |
| Bytes per position update | 4.4 | 8 | 8 |
| Memory per entity (stored state) | 54 B | 128 B | 128 B |
| Memory per player | 0.69 MB | 4 MB | 4 MB |
| Bytes per tick for a player who sees nothing that changed | 1 | 1 | 1 |
| Same bytes with 1 and 8 workers | identical | identical | identical |

**The first two goals are not met.** The limits are regression guards, at about half of what the engine measures
today, so the benchmark fails if it gets clearly slower; the goals are what the next version is for.

The six values behind the first two rows, and the main times, per tick (milliseconds, unless marked):

| Execution | Updates per second, 1 thread | `Update`, 1 thread | `Update`, 8 workers | Efficiency | Typical case, 1 thread |
|---|---|---|---|---|---|
| 1 | 5.75 million | 33.4 | 9.5 | 44% | 25.5 |
| 2 | 6.02 million | 31.9 | 10.0 | 40% | 24.7 |
| 3 | 5.99 million | 32.1 | 9.3 | 43% | 23.5 |
| 4 | 5.50 million | 34.9 | 9.8 | 44% | 24.6 |
| 5 | 5.42 million | 35.4 | 9.3 | 48% | 30.5 |
| 6 | 5.81 million | 33.1 | 10.0 | 41% | 23.2 |

Other numbers from the same runs (medians, in milliseconds per tick unless marked):

| Case | Time |
|---|---|
| Quantizing the 200,000 entities that changed | 10.4 |
| Confirmations of one tick, all 128 players | 6.1 |
| Typical case: `Update` and confirmations, 1 thread | 24.6, for 96,000 records |
| Typical case: quantizing the entities that changed | 2.2 |
| Building and writing one whole position record on its own, data in the processor cache | 48 nanoseconds (47 to 51) |
| Writing only the bits of one position record, data in the processor cache | 28 nanoseconds (27 to 33) |

How to read this:
- **Two sets of measurements, and what changed between them.** A first set of six executions, taken while the
  machine was compiling another large project, gave 4.7 million updates per second and 48% efficiency; the
  numbers here were taken after that finished. An earlier version of this code, before a review found and fixed
  two defects (a confirmation shortcut that could leave a player on an intermediate state, and entities that
  never settled when the round trip was longer than the remembered sends), measured 6.5 million updates per
  second and 46% efficiency. Quantizing the entities, which did not change, took 18% longer now than then, so
  the real cost of the fix is less than the 11% difference in throughput. We will measure again on a quiet
  machine.
- **Where the time goes.** Before the engine kept an index of who sees what, it looked every changed entity up in
  every player's visible set: 25.6 million searches a tick, about 330 ms in all (the benchmark of that version
  is in the git history). With the index the same tick takes 33 ms.
  What remains is about 6 ms to mark who needs what, on one thread (estimated from the difference between one
  thread and eight), and about 173 nanoseconds per record in all (33 ms for 192,000 records, marking included),
  against 48 for building and writing one record alone with the data in cache.
- **What did not help.** Fetching the state of the entities ahead of use into the cache made no difference to the
  throughput in our test. We think the per-player state, about 90 MB for 128 players, is what does not fit in
  the cache, but we have not shown it with a profiler.
- **The typical case is three times larger than it needs to be.** A changed entity is sent on three ticks in a
  row until the confirmation of the first send comes back, so a tick writes 96,000 records where 32,000 carry
  new information. How often to repeat belongs to the layer that decides what goes first, not to this one.
- **Size.** The 4.4 bytes are the whole buffer divided by its records, so the 1-byte end marker of each buffer is
  included. In the typical case a record takes 3.9 bytes, because the entity moves less between confirmed states.
- **Memory.** The 54 bytes per entity are the stored state of the entity plus some room for the list of changes.
  The 0.69 MB per player includes an equal part of the index of who sees what.

Conditions: Release build (MSVC), the same desktop machine as above, Windows only so far, one observer per player
and no exclusion or inclusion groups. The machine was also running other programs, and the variation between executions
is large.

## 7. The whole network path

A server with real relevancy and replication, 128 clients and a simulated network between them, all in one
process. This is the first measurement of everything the server does in a tick, and of what it sends. Source:
[`bench/bench_network.cpp`](../bench/bench_network.cpp).

![Network phase time per tick: measured value, regression limit and goal](images/network-phase.svg)

The scenario: 128 players and 200,000 entities of four types seen from 100, 250, 400 and 600 m, in a world
4,500 m wide (the density of the relevancy benchmark). Players walk at 5 m/s and a tenth of them drive at 25 m/s.
A sixth of the entities change in every tick (five times a second at 30 Hz): each takes a step of 0.2 m, and one in
ten of those also changes its health. The network loses 5% of the packets it carries (1,200 bytes each) and delays
every message by one or two ticks in each direction, about 100 ms there and back on average. The players join on a network
that loses nothing and settle; the 460 ticks that are measured (a little over 15 seconds, so that the saved-up part
of the budget is used and a full 10-second window is left) come after 20 more with the loss on. The server runs with
its default budget: 100 kbps on average, 200 over 10 seconds, buffers of 1,100 bytes.

| Measure | Result (median of 4 executions, range) | Limit | Goal |
|---|---|---|---|
| Network phase per tick, 1 thread | 49.5 ms (49.0 to 50.5) | 100 ms | 3.3 ms |
| Network phase per tick, 8 workers | 13.9 ms (13.1 to 14.2) | 30 ms | 1 ms |
| Bandwidth per player, the last 5 seconds | 98.1 kbps | 105 kbps | 100 kbps |
| Bandwidth per player, the busiest 10 seconds | 173 kbps | 205 kbps | 200 kbps |
| Records written per change, with the budget off | 1.09 | 1.5 | |
| A new player receives everything it sees | 61 ticks | 90 ticks | |
| Memory per player (server and client together) | 1.8 MB (1.77 to 1.78) | 3.6 MB | |
| Client work per tick, per client | 12.8 us (12.6 to 13.2) | 60 us | |
| Every client sees what the server has, after joining | yes | yes | yes |
| ... and after the world stops changing | 68 ticks | | |
| Bytes sent with 1 and 8 workers (16 players, 20,000 entities, 40 ticks) | identical | identical | identical |

**The goals for the time of the phase are not met; the goals for the bytes are.** The limits are regression guards:
about twice what the path measures today for times and memory (the client's work is far below its limit, because
the limit was set while the machine was busier), so that the benchmark fails if it gets clearly
slower, and the budget's own promise (with a little room for headers and event batches) for the bandwidth and the
join. Bandwidth is measured over the last 5 seconds because the first seconds spend what the player saved up.

The four executions, per tick (milliseconds, unless marked). Of the phase, about 2.1 ms on one thread is
quantizing the entities that changed and the rest is the server's `Update`. Everything else in the table above,
and the numbers below, were identical in the four, because they do not depend on the machine:

| Execution | Phase, 1 thread | Phase, 8 workers | Client, one `Poll` (us) |
|---|---|---|---|
| 1 | 50.0 | 14.2 | 12.8 |
| 2 | 48.4 | 14.1 | 12.8 |
| 3 | 49.0 | 13.7 | 13.2 |
| 4 | 50.5 | 13.1 | 12.6 |

Other numbers from the same runs:

| Case | Value |
|---|---|
| Entities visible to a player | 1,752 on average, 1,851 at most |
| Bytes per player per second over the whole run, server to player | 147 kbps: 143 state buffers, 4.0 event batches |
| Bytes per player per second, player to server (acks) | 1.2 kbps |
| Records written per player per tick | 97, where about 290 entities changed |
| Records that carry every property | 19.8% |
| Wait before a resend that the server settled on | 4.1 ticks |
| Messages the network loses | 4.6% (a state buffer is one packet now) |
| Age of the newest state buffer a player has applied | 1.6 ticks on average |
| State buffers applied per client per tick | 0.95 |
| Share of what a player sees that is not the current state, with the budget | 70% |
| ... with only the paced resends and no budget | 31% |
| Ticks for 16 players to join a network with 5% loss and see everything | 33 |

How to read this:
- **Where the time goes.** The server writes 97 records per player per tick where it wrote 966 before the budget
  (see record 0010), and the phase takes about 50 ms where it took about 66 ms. Part of that drop is a quieter
  machine, so it is not a gain to count on. What the figures say is that writing and sending records is not where
  most of the time goes: relevancy, the protocol and the transport are. This scenario moves 33,000 entities per tick,
  about 48 times as many as the main relevancy scenario (700), and the work of relevancy grows with the entities
  that move. That is the next place to look, with a profiler.
- **Where the bytes go.** A changed entity is now written again after the player's round trip and a tick, not in
  every tick: 1.09 records per change when the budget is off, against 3.3 before. The budget then holds the rest to
  what the player was promised. One record in five is complete (19.8%): an entity that waited more than 32 ticks has lost its
  reference state and is sent in full.
- **What the budget costs.** About 290 entities change per tick for each player, which is about 280 kbps of new
  information, and the budget carries 100. The rest waits, and the entities that waited longest go first. So each
  entity reaches the player about a third as often as it changes, and 70% of what a player sees is not the
  current state at any moment, against 31% when only the delay of the network and the waits make a copy old. The
  players still converge: 68 ticks after the world stops changing they hold exactly what the server has. A world
  that changes less, or players with a larger budget, have a smaller gap. Giving priority to what is near the player
  would make the gap matter less, and is not built yet.
- **Eight workers.** They make the phase 3.4 times faster. The marking of what changed runs on one thread
  (section 6), and we have not looked at what else does.
- **Bytes counted** are those of the messages, without the headers of IP, UDP or a network library.
- **Joining.** A new player joins the running world on the lossy network, and gets everything it sees in 61 ticks
  (about two seconds), with its first state spread over buffers of at most 1,100 bytes instead of one of tens of
  packets. The 33 ticks in the table are for a small world of 16 players and 20,000 entities with the loss on from
  the start.

Conditions: Release build (MSVC), the same desktop machine as above, Windows only so far, one observer per player and
no groups. The programs on the machine during the measurements were closed. The speed of a short test that the benchmark prints for this purpose
(writing one position record) was 53 nanoseconds in a full run, against 47 when the machine is quiet, so the times
may be a little pessimistic.

## Reproduce

Windows, from the repository root (details and prerequisites for each benchmark are in
[`bench/README.md`](../bench/README.md)):

```powershell
# data core (section 4), relevancy (section 5), replicated state (section 6) and the network path (section 7): one
# program, a few minutes on the machine above; add the word `network` after the program name to run section 7 alone
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
