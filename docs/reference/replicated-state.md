# Replicated state reference

Replicated state turns what the server knows about an entity (where it is, its health, its flags) into a few
bytes for each player who can see it, and turns those bytes back into state on the player's side. It decides
*how* things are sent. Which entities a player can see is decided by the [relevancy engine](relevancy.md), and
splitting the bytes into packets and putting them on the network comes later.

New to the terms? Read [Concepts explained simply](../concepts.md) first. This part of the core library is
plain C++20 and does not depend on Godot. Include files are under `tessera/net/replication/`.

## The idea in one minute

1. For each type of entity you say which of its table columns are replicated and how precisely
   (a position to the centimetre, an angle in 1,024 steps, a health between 0 and 1,000).
2. Every tick the server turns the entities that changed into **quantized** state: small whole numbers. This is
   done once per entity, however many players see it.
3. For each player the server remembers the last state the player **confirmed** having received. It writes only
   what differs from that state, and only for entities the player sees and that need sending.
4. The player's side reads the bytes and rebuilds the state. If a packet is lost, nothing breaks: the next
   buffer is still written against what the player confirmed, so it fills the gap.
5. An entity that has not changed and is confirmed costs nothing: no bytes, and one bit test per player who sees it.

## Describing the properties

A `ReplicationSchema` holds, for each entity type, the list of replicated properties in the order they are
written. Build it once at start-up.

```cpp
ReplicationSchema schema;
schema.AddType(1, {PropertyDescriptor::Position(TESSERA_COLUMN(MobColumns, x), TESSERA_COLUMN(MobColumns, y),
                                                TESSERA_COLUMN(MobColumns, z), 0.01f, 5000.0f),
                   PropertyDescriptor::Angle(TESSERA_COLUMN(MobColumns, yaw), 10)});
```

`TESSERA_COLUMN(Columns, member)` names a column of the struct of arrays that an `EntityTable` keeps. A type has
between 1 and 64 properties and at most 32 words (128 bytes) of quantized state; `AddType` returns false and
says why if a property cannot work, if the type is out of range, or if it already has a schema.

| Property | Reads | Parameters | Sent as |
|---|---|---|---|
| `Position(x, y, z, step, range)` | three `float` columns | `step`: size of one step; `range`: values stay within -range..range | the small difference from the confirmed position, or the whole position when that is shorter |
| `Angle(column, bits)` | a `float` column in radians | `bits`: 1 to 16, steps around the circle | the short way round between two angles |
| `Integer(column, is_signed, min, max)` | an integer column of 1, 2, 4 or 8 bytes | the range of values | the difference, or the whole value when that is shorter |
| `Bits(column, width)` | an integer column | `width`: 1 to 64 flags | the flags that changed, or the whole value when that is shorter |
| `Enum(column, value_count)` | an integer column | at least 2 values | the value, in as few bits as the count needs |

Values outside the range are limited to the nearest allowed value (an enum column holds 0 up to the number of
values minus 1; anything else, a negative number included, becomes the last value), and a `float` that is not a number becomes the
property's zero value (the centre of a position, angle 0, the minimum of an integer). The error of a quantized
value is at most half a step.

`schema.Hash()` is a fingerprint of everything that decides what the bytes mean, so a client can tell that it was
built from a different schema. Where the server reads a value from is local layout and is not part of it.

## One tick

Everything in this list belongs to the `Network` phase of the [tick](core.md).

1. **Flag what changed.** After you write the replicated columns of an entity (and after you create one) call
   `table.MarkDirty(entity)`. An entity that was never flagged has no state and is not replicated.
2. **`engine.Refresh(table)`**, once for each table. It quantizes the flagged entities, in parallel by page, and
   remembers which ones really changed (a move that stays inside the same step changes nothing).
3. **`engine.ApplyVisibility(connection, events)`** with the events the relevancy engine produced for that
   player. An entity that enters is sent in full; one that leaves or is destroyed is forgotten.
4. **`engine.Update(tick)`** writes one buffer for every player, in parallel across players. Read it with
   `engine.Output(connection)` and send it.
5. **`engine.Confirm(connection, tick)`** when a player confirms that the buffer written for `tick` arrived.

A buffer is a run of records, one for each entity that needs sending, followed by an end marker. An entity needs
sending when it changed, or when it was sent and no send of it has been confirmed yet. One that changed is sent at
once; one that is only waiting for its confirmation is sent again after a wait, and what does not fit in the budget
waits for the next tick (see [Pacing and the budget](#pacing-and-the-budget)).

### Confirmations

The server remembers the last `max_unconfirmed_sends` times it sent each entity (8 by default), so a confirmation
that arrives after the entity was sent again still counts: confirming tick 7 makes the state sent at tick 7 the
confirmed one, and forgets the sends before it. A confirmation for a tick that was never sent, was already
confirmed, or is older than what is remembered does nothing. If the confirmed state is the same as the newest
send and nothing changed since, the player is up to date and the entity stops being sent.

Sending the very same state again on the next tick does not take a new place, so an entity that does not change
is never forgotten, however long the round trip. An entity that changes on every tick needs a place for each
tick of the round trip: if the round trip is longer than `max_unconfirmed_sends` ticks, no confirmation matches
and the entity is sent in full every time. Raise the setting for players with a high ping.

If the confirmed state is more than `max_baseline_age_ticks` old (32 by default, about a second at 30 Hz), the
entity is sent in full instead, because the player's side may have forgotten it.

## Pacing and the budget

Two things keep the engine from sending a player more than the player can use: a wait before an entity is sent
again, and a budget of bytes.

**The wait.** An entity that changed, or that is new to the player, is a candidate at once. One that was sent, has
not been confirmed and has not changed since waits before it is a candidate again. The wait is the player's round
trip, measured in ticks and averaged (each new measurement counts for an eighth), plus `resend_margin_ticks`, kept
between `min_resend_ticks` and `max_resend_ticks`. Until there is a measurement it is `initial_resend_ticks`. The
code that runs the network reports each measurement with `engine.NoteRoundTrip(connection, ticks)`; the
[server](networking.md) does it for you. `engine.ResendAfterTicks(connection)` tells you the current wait. The
resolution is one tick, so a round trip of 1.4 ticks counts as 2.

**The budget.** Each player has a bucket of bytes. It fills at `rate_kbps` (kilobits per second, 0 for no limit)
and it can hold what the player is allowed to save up: the difference between `peak_kbps` and the rate, over
10 seconds. Over any 10 seconds a player gets at most `peak_kbps` on average. The bucket starts full, so a player
who joins gets a quick start. Every buffer takes its size from the bucket, plus 12 bytes for the headers of the
packet. `engine.Charge(connection, bytes)` takes out what was sent outside the buffer (the server uses it for event
batches), and the bucket may go into debt: the player then gets nothing until it is paid. A buffer is also never
larger than `max_buffer_bytes`, so that it fits in one packet.

**What goes first.** If every candidate fits, all are written. If not, each candidate gets a score, the number of
ticks it has been ready to send, counting the current one, times the weight of its type (`type_weight`, 1 to 255,
all 1 by default). The highest scores are written first; ties go to the lower compact id. An entity that does not
fit is skipped, and a smaller one after it may still fit. The records are then written in id order, as the format
needs. The size of a record is estimated from the last one written for that entity, and from the average of the
complete records of its type for a new one. The limit itself is exact: a record that would cross it is taken
back out of the buffer.

**What it costs the player.** Nothing is dropped. An entity that did not fit stays a candidate and waits longer,
so it goes first in the next tick. The budget promises bytes, not freshness: if the world changes more than the
player's budget can carry, each entity reaches the player less often, and the player's copy is behind. When the
world stops changing, the copy catches up. `LastStats()` reports, besides the records written, how many entities
were `waiting` for their resend and how many were `deferred` because they did not fit.

The example below gives each player 8 kilobits per second for 300 entities, much less than they need at once.
After a second the player has some of them, and after ten seconds all of them.

```cpp
#include <cstdint>
#include <span>

#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "test_support.hpp"

using namespace tessera::net;

struct MobColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
};

ReplicationSchema schema;
schema.AddType(1,
               {PropertyDescriptor::Position(TESSERA_COLUMN(MobColumns, x), TESSERA_COLUMN(MobColumns, y),
                                             TESSERA_COLUMN(MobColumns, z), 0.01f, 5000.0f)});

EntityTable<MobColumns> table(TableConfig{0, 1});
SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
RelevancyConfig relevancy;
relevancy.types[1] = TypeRelevancy{RelevancyMode::Spatial, 150.0f, 10.0f};

NetworkConditions conditions;
conditions.loss = 0.05;
conditions.max_delay_ticks = 2;
SimulatedNetwork network(conditions);

// The budget. Each player gets 8 kilobits a second, with no room to save up for bursts (the peak is the
// rate), in buffers of at most 600 bytes. An entity that was sent and not confirmed waits for the
// player's round trip plus one tick before it is sent again, and entities of type 1 count four times as
// much as the rest when there is not room for everything.
ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
RelevancyGroups groups;
ServerConfig config;
config.max_connections = 8;
config.max_visible_per_connection = 1024;
config.rate_kbps = 8;
config.peak_kbps = 8;
config.max_buffer_bytes = 600;
config.resend_margin_ticks = 1;
config.type_weight[1] = 4;
ReplicationServer server(config, relevancy, schema, grid, groups, jobs, network.Server());

Observer eye{0.0f, 0.0f, 0.0f};
server.SetCallbacks(
    [&](ConnectionId id) { server.Relevancy().SetObservers(id, std::span<const Observer>(&eye, 1)); },
    [](ConnectionId) {});

// 300 mobs around the player: more than the budget can send in the first second.
for (uint32_t i = 0; i < 300; ++i) {
    const EntityId mob = table.Create();
    const auto where = table.Find(mob);
    MobColumns& columns = table.Columns(where->page);
    columns.x[where->slot] = static_cast<float>(i % 20) * 5.0f;
    columns.y[where->slot] = 0.0f;
    columns.z[where->slot] = static_cast<float>(i / 20) * 5.0f;
    table.MarkDirty(mob);
    grid.Insert(mob, columns.x[where->slot], 0.0f, columns.z[where->slot]);
}

ClientReplica client(*network.AddClient(), schema);
uint32_t seen_after_one_second = 0;
for (uint64_t tick = 0; tick < 300; ++tick) {
    network.Advance(tick);
    client.Poll();
    server.Refresh(table);
    server.Update(tick);
    if (tick == 29) {
        client.ForEachEntity([&](EntityId, const uint32_t*) { ++seen_after_one_second; });
    }
}

// The budget held some entities back at first, and in the end the player has all of them.
uint32_t seen = 0;
client.ForEachEntity([&](EntityId, const uint32_t*) { ++seen; });
TESSERA_CHECK(seen_after_one_second < 300);
TESSERA_CHECK(seen == 300);
const uint32_t wait = server.Replication().ResendAfterTicks(client.ConnectionId()); // measured, in ticks
TESSERA_CHECK(wait >= config.min_resend_ticks && wait <= config.max_resend_ticks);
```

The code above is the body of a test (`tests/test_budget_reference_example.cpp`), so it is compiled and run with
every build.

## The player's side

`ReceiverState` turns buffers back into state. It is also the reference for the client that will be built on it.

```cpp
ReceiverState receiver(schema, max_visible, config.max_baseline_age_ticks);
receiver.OnEnter(compact_id, entity, tick);   // an Enter event, with the tick it was produced in
receiver.AdvanceEvents(tick);                 // all events up to this tick have been applied
DecodeStatus status = receiver.Apply(buffer, tick);
const uint32_t* state = receiver.Current(compact_id);
```

Two rules keep a bad network from corrupting state:

- **Visibility events are applied in the order the server produced them, each with its tick, and `AdvanceEvents`
  is called after each batch (empty ones included).** A buffer for a tick the events have not reached yet is
  refused with `EventsMissing`, because the compact ids in it may already stand for other entities on the
  server. Keep it and try again when the events arrive.
- A buffer from before an entity became visible under its compact id is refused with `StaleRecord`: that number
  belonged to another entity then.

The receiver keeps the states it received over the last `history_ticks` ticks for each entity, because a record
is written against the state the server had *confirmed*, which can be older than the newest one received.
Records are applied whole or not at all, and a late older record is kept as history without replacing a newer
state.

| `DecodeStatus` | Meaning |
|---|---|
| `Ok` | The whole buffer was applied. It is safe to confirm it. |
| `Truncated` | The buffer ended early. |
| `BadRecord` | A value was out of its range or a number was impossible. |
| `UnknownEntity` | A record named a compact id the receiver was not told about. |
| `MissingBaseline` | A record is written against a state the receiver does not have. |
| `StaleRecord` | The buffer is from before the entity entered. |
| `EventsMissing` | The events of the buffer's tick have not been applied yet. |

When `Apply` stops at a bad record, the records before it stay applied and `applied` (if you pass it) says how
many. Do not confirm a buffer unless the status is `Ok`.

## Configuration

| Field | Default | What it does |
|---|---|---|
| `max_connections` | 128 | Highest number of players at once, at most 65,535. |
| `max_visible_per_connection` | 65,536 | Compact ids per player. Use the same number as the relevancy engine. |
| `max_baseline_age_ticks` | 32 | Oldest confirmed state still used as a reference. The receiver must keep at least this many ticks. |
| `max_unconfirmed_sends` | 8 | Recent sends remembered for each entity and player, 1 to 255. It should cover the round-trip time in ticks. |
| `tick_rate` | 30 | Ticks per second, 1 to 1,000. Turns kilobits per second into bytes per tick. |
| `rate_kbps` | 0 | The average a player may be sent, in kilobits per second, up to 1,000,000. 0 means no limit, and then `peak_kbps` and `max_buffer_bytes` do nothing. |
| `peak_kbps` | 0 | The most a player may be sent on average over 10 seconds. 0 means the rate; otherwise from the rate up to 1,000,000. |
| `max_buffer_bytes` | 65,535 | The largest buffer, 64 to 65,535 when there is a rate. It must hold the largest complete record of the schema. |
| `resend_margin_ticks` | 1 | Added to the measured round trip, 0 to 30. |
| `min_resend_ticks`, `max_resend_ticks` | 1, 30 | The shortest and longest wait, 1 to 255. |
| `initial_resend_ticks` | 1 | The wait before a round trip has been measured. |
| `type_weight[type]` | 1 for every type | How much a type counts when there is not room for everything, 1 to 255. |

The [server](networking.md) has its own defaults for these (a rate of 100 and a peak of 200 kilobits per second, and a
buffer of 1,100 bytes), because it is the one that talks to real players.

The engine's constructor throws `std::invalid_argument`, naming the field, when a value is zero or too large.

## The engine

| Call | What it does |
|---|---|
| `AddConnection(id)` / `RemoveConnection(id)` | Add or remove a player. Returns false for an id out of range, already in use, or unknown. Removing frees everything kept for them. |
| `Refresh(table)` | Quantize the flagged entities of one table. Returns how many changed. |
| `ApplyVisibility(id, events)` | Apply the relevancy events of one player. Unknown players are ignored. |
| `Update(tick)` | Write a buffer for every player. Call once per tick, with a `tick` that grows. |
| `Output(id)` | The buffer of the last `Update`; empty for an unknown player. |
| `Confirm(id, tick)` | The player confirmed the buffer written for `tick`. |
| `NoteRoundTrip(id, ticks)` | A confirmation took `ticks` ticks to come back; the wait before a resend follows the average. |
| `Charge(id, bytes)` | Take bytes out of a player's budget for something sent outside the buffer. It may go into debt. |
| `ResendAfterTicks(id)`, `BudgetBytes(id)` | The current wait, and the bytes the player may be sent right now. |
| `Store()`, `Connection(id)` | Read-only views, for diagnostics and tests. |
| `LastStats()` | Records written by the last `Update` (and how many were complete), and how many entities were waiting or did not fit, added up over players. |
| `SubscriptionCount()` | How many (entity, player) pairs are being tracked. |

Entities are looked up by type and index, so all the tables of one engine must use the same shard.

Every entity that changed is looked up in an index of who sees it, so the cost of marking grows with the players
who really see it and not with all of them.

## The record format

A buffer is written bit by bit, from the least significant bit of each byte. Numbers marked *var* take 4 bits for
each 3 bits of value, so small numbers are cheap.

| Field | Size | Meaning |
|---|---|---|
| gap | var | The compact id minus the previous record's (the first counts from -1). At least 1; 0 ends the buffer. |
| complete | 1 bit | 1: every property follows in full. 0: only what differs from a confirmed state. |
| age - 1 | var | (partial only) how many ticks before this one the confirmed state was sent, minus 1. |
| mask | 1 bit per property | (partial only) 1 for each property that follows. |
| values | per property | Each property that follows, written by its kind. |

A record that is partial with an empty mask means "the same as the confirmed state": it is sent when an entity
changed and then went back to what the player had confirmed, while the intermediate state may have arrived.

## What it costs

Measured numbers, with the machine and the code that produced them, are on the
[benchmarks page](../benchmarks.md#6-replicated-state). With 128 players who each see 1,500 of 200,000 entities:

- A position update takes about 4.4 bytes, including its id and the property mask.
- A stored entity takes about 54 bytes, and each player about 0.7 MB.
- A player who sees only entities that have not changed gets one byte per tick.
- On one thread the engine writes about 5.8 million updates per second. **This is below the goal of 10 million
  per second**, and with eight workers it uses about 44% of them, below the goal of 70%. The decisions behind
  the design, and what is left to improve, are in [record 0009](../adr/0009-replicated-state.md).

## Coming next

The format leaves room for more kinds of property, and adding one only adds a codec; records of the kinds above
do not change. On the list: scalar floats (quantized and full precision), 2D and 3D vectors, quaternions, colours,
references to other entities, short strings, byte blobs, arrays and lists, nested structures, optional values
and composite transforms. Values whose size varies will be compared by a revision number, so each player's saved
state stays the same size.

## Example

```cpp
#include <cstdint>
#include <span>

#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication/receiver_state.hpp"
#include "tessera/net/replication/replication_engine.hpp"

using namespace tessera::net;

// 1. The columns of one entity type, as its entity table keeps them.
struct MobColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
    float yaw[kPageSlots];
    int32_t health[kPageSlots];
};

// 2. Say which properties of type 1 are replicated, and how precisely.
ReplicationSchema schema;
schema.AddType(
    1, {PropertyDescriptor::Position(TESSERA_COLUMN(MobColumns, x), TESSERA_COLUMN(MobColumns, y),
                                     TESSERA_COLUMN(MobColumns, z), 0.01f,
                                     5000.0f), // 1 cm steps, up to 5 km from the origin
        PropertyDescriptor::Angle(TESSERA_COLUMN(MobColumns, yaw), 10), // 1,024 steps around the circle
        PropertyDescriptor::Integer(TESSERA_COLUMN(MobColumns, health), true, 0, 1000)});

// 3. The engine, a table for the entities, and one connected player.
ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
ReplicationConfig config;
config.max_connections = 8;
config.max_visible_per_connection = 1024;
ReplicationEngine engine(schema, config, jobs);
EntityTable<MobColumns> table(TableConfig{0, 1});
engine.AddConnection(0);

// 4. Create an entity, write its columns, and flag it as changed.
const EntityId mob = table.Create();
const auto where = table.Find(mob);
MobColumns& columns = table.Columns(where->page);
columns.x[where->slot] = 120.5f;
columns.y[where->slot] = 0.0f;
columns.z[where->slot] = -40.25f;
columns.yaw[where->slot] = 1.0f;
columns.health[where->slot] = 80;
table.MarkDirty(mob);

// 5. Every tick, in the Network phase: quantize what changed, tell the engine what each player can see
//    (these events come from the relevancy engine), and write a buffer for each player.
const uint64_t tick = 1;
engine.Refresh(table);
const VisibilityEvent enter{VisibilityEvent::Kind::Enter, mob, 0};
engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter, 1));
engine.Update(tick);
const std::span<const uint8_t> buffer = engine.Output(0); // send this to the player

// 6. On the player's side, a receiver turns the buffer back into state.
ReceiverState receiver(schema, 1024, config.max_baseline_age_ticks);
receiver.OnEnter(0, mob, tick); // the same Enter event, delivered reliably and in order
receiver.AdvanceEvents(tick);
const DecodeStatus status = receiver.Apply(buffer, tick);
engine.Confirm(0, tick); // when the player confirms that the buffer arrived

const uint32_t* quantized = receiver.Current(0); // the position, angle and health as whole numbers
TESSERA_CHECK(status == DecodeStatus::Ok);
TESSERA_CHECK(quantized != nullptr &&
              quantized[0] == 512'050); // x = 120.5 m is 512,050 steps from -5,000 m
```

The code above is the body of a test (`tests/test_replication_reference_example.cpp`), so it is compiled and run
with every build. In a real server the events come from the [relevancy engine](relevancy.md) and `tick` from the
[tick loop](core.md).
