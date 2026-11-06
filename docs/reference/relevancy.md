# Relevancy reference

Relevancy answers one question for every connected player, every tick: *which entities does this player
need to know about?* The relevancy engine (`RelevancyEngine`) answers it and reports what changed as
events. It does not send anything over the network yet; it is the part that decides what will be sent.

New to the terms? Read [Concepts explained simply](../concepts.md) first. The engine is part of the core
library, so it is plain C++20 and does not depend on Godot. Include files are under
`tessera/net/relevancy/`.

## How it decides

What a player sees is worked out in three steps:

1. **Start from the entity's type.** Each type of entity (0 to 255) has one mode:
   - `Spatial`: visible when it is within `range` of one of the player's observers (the points the player
     looks from; usually one camera, but a player can have several).
   - `AlwaysRelevant`: visible to every player, wherever they are.
   - `OwnerOnly`: visible only to the player that owns it (a backpack, for example).
2. **Add what inclusion groups bring.** If the entity is in an inclusion group that allows this player, the
   player sees it even if its type would hide it.
3. **Remove what exclusion groups hide.** If the entity is in an exclusion group that does not allow this
   player, the player does not see it. This step comes last, so nothing overrides it.

Groups are how the game makes exceptions: "hide this squad's loot from other teams" is an exclusion group,
"show this objective to the whole team" is an inclusion group.

## Configuration

`RelevancyConfig` holds the settings. The defaults are meant to be a good start.

| Field | Default | What it does |
|---|---|---|
| `max_connections` | 128 | Highest number of players at once. Connection ids go from 0 to this number minus 1. |
| `recalc_interval_ticks` | 3 | Each player re-checks what they see once every this many ticks. Players are spread across ticks, so the work is even. A higher number costs less and delays the moment an entity appears by up to that many ticks (about 100 ms at 30 Hz with the default). |
| `max_observers_per_connection` | 4 | Most observers one player can have. |
| `max_visible_per_connection` | 65,536 | Most entities one player can see at once. |
| `types[n]` | `Spatial`, range 0 | The rule for entity type `n`: `mode`, `range` and `hysteresis_margin`. A range of 0 means "never visible". |

The **hysteresis margin** is extra distance in which an entity that is already visible stays visible. It stops
an entity standing at the edge of the range from appearing and disappearing every tick. An entity enters at
`range` and leaves beyond `range + hysteresis_margin`.

`ValidateRelevancyConfig` (called by the engine's constructor) throws `std::invalid_argument`, naming the
field, when a value cannot work: a zero count, a negative or non-finite range, or more than 16,777,216
visible entities per player.

## The engine

`RelevancyEngine(config, grid, groups, jobs)` reads positions from a [`SpatialGrid`](core.md) and uses an
[`IJobSystem`](core.md) to work on many players at once. The grid, the groups and the job system must outlive
the engine.

| Call | What it does |
|---|---|
| `AddConnection(id)` / `RemoveConnection(id)` | Add or remove a player. Returns false for an id out of range, already in use, or unknown. Removing a player frees everything the engine kept for them, and the `OwnerOnly` entities they owned are left without an owner. |
| `SetObservers(id, observers)` | Replace the points the player looks from. Returns false, changing nothing, for an unknown player, too many observers, or a coordinate that is not a finite number. An empty list is allowed: the player then sees no spatial entities. |
| `OnEntityCreated(entity)` | Tell the engine an entity exists. Needed for `AlwaysRelevant` and `OwnerOnly` types. Spatial entities are found through the grid. |
| `OnEntityDestroyed(entity)` | Tell the engine an entity is gone. It must no longer be in the grid when `Update` runs. |
| `SetOwner(entity, id)` | Set the owner of an `OwnerOnly` entity. Returns false if the entity is unknown, its type is not `OwnerOnly`, or the player does not exist. |
| `Update(tick)` | Work out what each player sees now. Call once per tick. |
| `Events(id)` / `ClearEvents(id)` | Read, then clear, what changed for a player. |
| `CompactIdOf(id, entity)` | The short id of an entity for a player, or `kInvalidCompactId` if they cannot see it. |
| `VisibleCount(id)`, `VisibleOverflow(id)` | How many entities the player sees, and how many times an entity was kept out because the ids ran out (added up over every re-check). |

Everything that changes the set of things (creating or destroying entities, owners, groups, players,
observers) belongs in the `Structure` phase of the [tick](core.md). `Update` belongs in the `Network` phase.
During `Update` the engine only reads the grid and the groups, which is why it can check many players in
parallel without locks.

### Events and compact ids

For each change the engine appends a `VisibilityEvent` to the player's list:

| Kind | Meaning |
|---|---|
| `Enter` | The player can now see the entity. `compact_id` is the short id to use for it. |
| `Leave` | The entity is out of sight but still exists. |
| `Destroy` | The entity no longer exists. |

Within one `Update`, events come as `Destroy`, then `Leave`, then `Enter`, each ordered by entity id, so the
result is the same with one worker or many. Destructions are reported in the very next `Update` for every
player, whatever their re-check schedule.

A *compact id* is a small number (from 0 up to `max_visible_per_connection` minus 1) that stands for an entity on one
player's connection. It is much cheaper to send than the 64-bit entity id. When an entity leaves, its compact
id is not given to another entity until the following tick, so the player's machine always has a tick to
process "that one left" before the number means something else. If a player already sees as many entities as
there are ids, the others stay hidden until an id frees up, and `VisibleOverflow` counts each time that happens.

### Groups

```cpp
RelevancyGroups groups;
GroupId secret = groups.CreateGroup(GroupKind::Exclusion);
groups.AddMember(secret, crate);
groups.SetStatus(secret, 0, true);   // connection 0 may see it; everyone else may not
```

A group is either `Inclusion` or `Exclusion`. Each group has a *status* per player: allowed or not allowed.
Until you say otherwise nobody is allowed, so an exclusion group hides its members from everyone and an
inclusion group shows them to no one. `SetStatus(group, id, allowed)` changes one player and
`SetStatusForAll(group, allowed)` changes everyone. An entity can belong to several groups.

When you destroy an entity or remove a player the engine removes them from every group, so a later entity
or player that reuses the same number starts clean.

## Example

```cpp
#include <span>

#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_engine.hpp"
#include "tessera/net/spatial_grid.hpp"

using namespace tessera::net;

// 1. Say how far each type of entity can be seen.
RelevancyConfig config;
config.max_connections = 64;
config.types[1] = {RelevancyMode::Spatial, 150.0f, 10.0f};      // type 1: crates, seen from 150 m
config.types[2] = {RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};  // type 2: the world clock
config.types[3] = {RelevancyMode::OwnerOnly, 0.0f, 0.0f};       // type 3: a player's backpack

SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
RelevancyGroups groups;
ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
RelevancyEngine engine(config, grid, groups, jobs);

// 2. A player connects and looks from one point.
engine.AddConnection(0);
const Observer camera{10.0f, 0.0f, 10.0f};
engine.SetObservers(0, std::span<const Observer>(&camera, 1));

// 3. The world has a crate nearby and a clock.
const EntityId crate = EntityId::Make(0, 1, 7, 0);
grid.Insert(crate, 30.0f, 0.0f, 30.0f);
const EntityId clock = EntityId::Make(0, 2, 1, 0);
engine.OnEntityCreated(clock);

// 4. Every tick, in the Network phase:
const uint64_t tick = 0;
engine.Update(tick);
size_t entered = 0;
for (const VisibilityEvent& event : engine.Events(0).events) {
    if (event.kind == VisibilityEvent::Kind::Enter) {
        ++entered;  // start sending event.id using event.compact_id
    }
    // Leave and Destroy: stop sending it.
}
engine.ClearEvents(0);
```

The code above is the body of a test (`tests/test_relevancy_reference_example.cpp`), so it is compiled and run
with every build. In a real server `tick` comes from the [tick loop](core.md).

## What it costs

Each re-check asks the grid for everything within the widest range of any type plus its margin, then keeps what each type's
own range allows. So the cost grows with the number of players and with how many entities are packed into
that widest range, not with the size of the world. Measured numbers, with the machine and the code that
produced them, are on the [benchmarks page](../benchmarks.md#5-relevancy). In short: with 32 players in a
world of 250,000 entities one tick takes about 3 ms on one thread and 0.6 ms with eight workers, and with 128
players among a million entities about 14 ms and 2.3 ms. We allow 3.3 ms on one thread for the whole network
phase, so smaller servers fit when relevancy has several workers, and the largest server we aim for does not
fit yet. A faster design is the next step ([record 0007](../adr/0007-relevancy-first-version.md)).
