# Core library reference

The core library (`core/`) is plain C++20 and does not depend on Godot. Everything lives in the
`tessera::net` namespace; include files are under `tessera/net/`.

New to the terms? Read [Concepts explained simply](../concepts.md) first.

## How a tick works

The server runs in fixed steps called *ticks* (30 per second by default). Every tick runs five
*phases*, always in this order:

| Phase | What it is for |
|---|---|
| `Input` | Apply player input. |
| `Simulate` | Run the game simulation, spread over all CPU cores. |
| `Structure` | Create and destroy entities (the requests queued during `Simulate`). |
| `Grid` | Update the spatial grid. |
| `Network` | Decide what each player sees and send it. |

Entities are only created or destroyed in the `Structure` phase, when nothing else is running. That
is what lets every other phase run on many threads without locks.

## Classes

### `EntityId` (`entity_id.hpp`)
A 64-bit identity that is unique across the whole world: shard (12 bits), type (8), index (28) and
generation (16). The generation changes every time a slot is reused, so an old id can never be
mistaken for a new entity. `EntityId::Null()` refers to nothing. Generation `0xFFFF` is reserved as
the "retired" marker.

### `EntityTable<PageColumns>` (`entity_table.hpp`)
Stores all entities of one type. `PageColumns` is a struct with one array of `kPageSlots` (4096)
values per property:

```cpp
struct Columns {
    float x[kPageSlots];
    float y[kPageSlots];
};
EntityTable<Columns> table(TableConfig{});
EntityId id = table.Create();          // EntityId::Null() when the table is full (see FailedCreateCount())
const SlotLocation where = *table.Find(id);
table.Columns(where.page).x[where.slot] = 5.0f;
table.Destroy(id);                     // false for a stale or foreign id
```

The constructor throws `std::invalid_argument` if the type or shard does not fit in an `EntityId`. Slots never move, so an entity keeps its place for its whole life. A slot whose generation counter
runs out is retired and never reused. `MarkDirty` and `ConsumeDirtyInPage` track what changed, which
the replication layer uses to send only differences.

### `TypeRegistry` (`type_registry.hpp`)
Maps type numbers (0 to 255) to names. Fill it once at start-up.

### `IJobSystem` and `ThreadPoolJobSystem` (`job_system.hpp`)
`ParallelFor(count, grain, body)` splits `[0, count)` into blocks and runs `body(begin, end, worker)` on
all workers, returning when every block is done. The calling thread is worker 0.

### `CommandBuffers<Command>` (`command_buffer.hpp`)
One queue per worker, so many threads can queue create and destroy requests at the same time.
Every command has an *order key*. `ApplyAll` applies commands sorted by key, so the result is the same
whether you use 1 or 8 workers. A key identifies one source of commands (for example one input item)
among all commands pushed between two `ApplyAll` calls: several commands from one source share its key
and keep their order, but two different sources must never share a key. Do not call `Push` from inside
the `ApplyAll` callback.

### `SpatialGrid` (`spatial_grid.hpp`)
A sparse 3D grid. Cell size is set per axis (Y is vertical). `QueryCell`, `QueryBox` and
`QuerySphere` answer "who is near here?". `WouldChangeCell` can be called from many threads to find
the entities that crossed a cell border; `ApplyMoves` then updates the grid in one step.

The grid stores the position of each entity as of the last `ApplyMoves`. `QueryBox` and `QuerySphere`
filter on those stored positions, so if you need exact answers, pass a `PendingMove` for every entity
that moved, not only for those that crossed a cell border (`ApplyMoves` reports how many changed cell).

### `TickLoop` (`tick_loop.hpp`)
Runs the phases at a fixed rate and records timing (`Metrics()`). If a tick takes longer than its
budget the next one starts right away and the schedule restarts; ticks are never run back to back to
"catch up". An optional `ILoadPolicy` is told how long every tick took.

## Minimal example

```cpp
#include <atomic>

#include "tessera/net/command_buffer.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/tick_loop.hpp"

using namespace tessera::net;

struct Columns {
    float x[kPageSlots];
};

int main() {
    ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
    EntityTable<Columns> table(TableConfig{});
    CommandBuffers<StructuralCommand> commands(jobs.WorkerCount());
    TickLoop loop(TickConfig{}, jobs);  // 30 ticks per second

    std::atomic<bool> stop{false};

    loop.SetPhaseHandler(Phase::Simulate, [&](PhaseContext& context) {
        // Ask for one new entity per item. Nothing is created yet.
        context.jobs.ParallelFor(100, 10, [&](uint32_t begin, uint32_t end, uint32_t worker) {
            for (uint32_t item = begin; item < end; ++item) {
                commands.Push(worker, item, StructuralCommand{StructuralCommand::Kind::Create, 0, EntityId::Null()});
            }
        });
    });

    loop.SetPhaseHandler(Phase::Structure, [&](PhaseContext& context) {
        // Now it is safe to change the table.
        commands.ApplyAll([&](const StructuralCommand& command) {
            if (command.kind == StructuralCommand::Kind::Create) {
                table.Create();
            }
        });
        if (context.tick == 29) {
            stop.store(true);  // stop after one second
        }
    });

    loop.Run(stop);
    return 0;
}
```

## Measured performance

The acceptance benchmark (`tessera_net_bench`, built with `-DTESSERA_BUILD_BENCH=ON`) checks the
numbers below with 1,000,000 entities on a desktop CPU (12 cores). Your numbers will differ.

| What | Result |
|---|---|
| Create 1 M entities | about 12 ms |
| Destroy 1 M entities | about 6 ms |
| Iterate 1 M entities | about 1.5 ms per pass |
| Memory per entity (bookkeeping) | 4.25 bytes |
| Speed-up with 8 workers (CPU-bound work) | about 91 to 95% efficiency |
| Tick jitter at 30 Hz, p99 | below 0.1 ms |
| Same result with 1 and 8 workers | yes (1,000 ticks) |
