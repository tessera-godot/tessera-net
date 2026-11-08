# Networking reference

This is where the pieces meet. The [relevancy engine](relevancy.md) decides what each player sees, the
[replicated state](replicated-state.md) engine turns it into bytes, and this part moves those bytes between a
server and its players: a transport interface, a simulated network to test against, five messages, a
`ReplicationServer` that runs the whole Network phase of a tick, and a `ClientReplica` that applies what arrives.

New to the terms? Read [Concepts explained simply](../concepts.md) first. This part of the core library is plain
C++20 and does not depend on Godot. Include files are under `tessera/net/`, and the transport ones under
`tessera/net/transport/`.

## The idea in one minute

1. The server and each player talk through a **transport**: something that sends and receives whole messages on
   two channels, one that always delivers in order and one that may lose, repeat or reorder.
2. A player says hello with the version of the protocol and a fingerprint of the replicated schema. The server
   accepts or says why not.
3. Each tick the server first tells each player what became visible or invisible (**event batches**, reliable,
   numbered), then sends what changed (**state buffers**, unreliable).
4. The player applies the events in order and the state buffers whole or not at all, and confirms what it applied
   (**acks**, unreliable).
5. Nothing needs a retry timer. A state buffer that is lost is covered by the next one, because the server keeps
   sending whatever the player has not confirmed.

## The layers

```
 ReplicationServer          ClientReplica
   relevancy + replication     applies events and state
   protocol (5 messages)       protocol
        |                          |
     ITransport  <--- network --->  ITransport
```

The server and the client only know `ITransport`. Today there is one implementation, `SimulatedNetwork`, which
lives in memory. A transport over real sockets is a later step; it has to respect the contract below, and nothing
above it changes.

## The transport

```cpp
class ITransport {
public:
    virtual void Poll(std::vector<TransportEvent>& out) = 0;   // append what happened since the last call
    virtual bool Send(TransportConnection, Channel, std::span<const uint8_t>) = 0;
    virtual void Disconnect(TransportConnection) = 0;
    virtual TransportStats Stats() const noexcept = 0;
};
```

A `TransportEvent` is `Connected`, `Disconnected` or `Message` (with its connection, channel and bytes). The
unit is a **message**: whole, with its length, never a piece of one. A transport that has to cut large messages
into packets does it inside and puts them back together.

| Channel | Contract |
|---|---|
| `ReliableOrdered` | Arrives, in the order it was sent, or the connection fails and both sides are told. |
| `Unreliable` | May be lost, repeated or arrive out of order. A message that arrives is whole and has the bytes that were sent. |

Both `Send` and `Disconnect` on a connection that is not open do nothing (`Send` returns false). The server sees
each player as a `TransportConnection` number, starting at 1 and never reused; the player sees the server as
connection 0.

### What the contract does not cover

- **Integrity and authentication.** A message that arrives is assumed to be the one that was sent. A forged but
  well-formed message cannot be told from a real one: a state buffer that claims a tick it does not hold could
  make a player confirm too much. (An ack for a tick the server has not sent is dropped and counted in
  `messages_ignored`, so a player cannot stall its own confirmations.) Checking packets is the job of the
  transport that carries them; the networking libraries commonly used with games do it.
- **Encryption and abuse.** Not handled here.

## The simulated network

`SimulatedNetwork` is a whole network in memory, driven by the tick you give it, so tests and benchmarks run the
real server and the real clients over a bad connection and get the same result every time.

```cpp
NetworkConditions conditions;
conditions.loss = 0.05;            // chance that a packet is lost
conditions.duplicate = 0.01;       // chance that an unreliable message arrives twice
conditions.min_delay_ticks = 1;
conditions.max_delay_ticks = 2;    // each message takes 1 to 2 ticks, chosen at random
conditions.mtu = 1200;             // bytes in a packet
conditions.seed = 7;
SimulatedNetwork network(conditions);

ITransport& server_end = network.Server();
ITransport* client_end = network.AddClient();
network.Advance(tick);             // deliver everything due at this tick
```

What it models:

- **Loss per packet.** A message of `n` bytes needs `ceil(n / mtu)` packets and arrives only if all of them do,
  so a large message is lost far more often than a small one: at 5% loss, a message of three packets is lost
  14% of the time and one of sixty-eight, 97%.
- **Duplicates and delay.** Unreliable messages may arrive twice and out of order, because each takes its own
  random delay.
- **Reliable ordered delivery.** Every reliable message arrives, after the one before it. The simulator does not
  model the delay a retransmission would add.
- **Disconnects.** Either side can close; the other is told, once, after what was already on its way.
- **Repeatable.** The same seed and the same calls give the same bytes in the same order.

What it does not model: bandwidth limits and queues, bursts of loss, jitter that depends on load, packet
headers (the byte counts are the messages', without IP or UDP headers), and the retransmission timing of a real
reliable channel. The loss rule for large messages is an approximation of how a reliable-over-UDP library treats
fragments, so convergence times on a real network may differ.

## The five messages

Every message starts with one byte that says which it is. Whole numbers take 4 bits for each 3 bits of value, so
small ones are cheap. The decoders refuse a message that is truncated, has bytes left over or holds a value that
cannot be right.

| Message | Direction, channel | Carries |
|---|---|---|
| `ClientHello` | player to server, reliable | The protocol version and the hash of the schema. |
| `ServerHello` | server to player, reliable | Accepted, or the reason (`VersionMismatch`, `SchemaMismatch`, `ServerFull`); if accepted, the player's connection id, the tick rate, how many compact ids it may use and how many ticks of history it keeps. |
| `EventBatch` | server to player, reliable | A number (1, 2, 3, ...), the tick, and the visibility events of that tick. Sent only when there are events. |
| `StateBuffer` | server to player, unreliable | The tick, how many event batches the server had sent when it wrote it, and the buffer from the replication engine, unchanged. Sent only when it holds a record. |
| `Ack` | player to server, unreliable | The newest tick applied and a mask of the 32 before it. |

## One tick on the server

`ReplicationServer::Update(tick)` is the whole Network phase. In order:

1. **Read the transport.** New players are greeted (`ClientHello` checked against the version, the schema hash
   and the free slots), players that left are removed, and each ack confirms to the replication engine the ticks
   it confirms for the first time.
2. **Relevancy.** The relevancy engine updates what each player sees.
3. **Event batches.** For each player with events, one numbered `EventBatch` goes out on the reliable channel,
   and the same events go to the replication engine.
4. **Replication.** The engine writes one buffer per player.
5. **State buffers.** Each buffer that holds a record goes out as a `StateBuffer` on the unreliable channel, with
   the number of event batches sent so far. A player with nothing to receive, or whose budget is empty, gets no
   message at all.

Before `Update`, call `server.Refresh(table)` for each table, after you have written the replicated columns and
called `table.MarkDirty(entity)`. This is the "quantize what changed" step of [replicated state](replicated-state.md).
It is separate so that you can time it, and so that you can call it once per table.

The server owns the relevancy and replication engines. It overrides `max_connections` and
`max_visible_per_connection` in the configuration you give the relevancy engine, so that both engines and the
protocol agree. Tell the relevancy engine where a player looks from (`server.Relevancy().SetObservers`) when the
player connects; the `on_connected` callback is the place.

| Field of `ServerConfig` | Default | What it does |
|---|---|---|
| `max_connections` | 128 | Players at once. |
| `max_visible_per_connection` | 65,536 | Compact ids per player, 1 to 262,144. |
| `tick_rate` | 30 | Told to the players in the hello, 1 to 1,000. |
| `max_baseline_age_ticks` | 32 | See [replicated state](replicated-state.md#configuration), 1 to 255. |
| `max_unconfirmed_sends` | 8 | The same. |
| `rate_kbps` | 100 | Kilobits per second a player may be sent on average, up to 1,000,000. 0 means no limit. |
| `peak_kbps` | 200 | The most on average over any 10 seconds, from the rate up to 1,000,000. 0 means the rate. |
| `max_buffer_bytes` | 1,100 | The largest state buffer, 64 to 65,535. It fits one packet of the usual 1,200 bytes. |
| `resend_margin_ticks` | 1 | Added to the measured round trip before an unconfirmed entity is sent again, 0 to 30. |
| `min_resend_ticks`, `max_resend_ticks` | 2, 30 | The shortest and the longest wait, 1 to 255. |
| `initial_resend_ticks` | 6 | The wait until the first round trip of a player has been measured. |
| `type_weight[type]` | 1 for every type | How much a type counts when there is not room for everything, 1 to 255. |

The pacing and the budget are described in [replicated state](replicated-state.md#pacing-and-the-budget). The server
measures each player's round trip itself: the ticks between writing a buffer and the first ack that confirms it
(only the newest tick of an ack counts, because the older ones were confirmed late by design), and it takes the
event batches it sends out of the player's budget too.

The constructor throws `std::invalid_argument`, naming the field, for a value out of range, and for an entity type
that players can see (a spatial range above zero, always relevant, or owner only) but that has no schema.

A player who breaks the protocol on the reliable channel (a message that cannot be read, or one a player never
sends) is disconnected and counted in `Stats().protocol_errors`. The same on the unreliable channel is only
dropped and counted in `malformed_messages`. An ack before the hello is ignored and counted in `messages_ignored`.

## The player's side

```cpp
ClientReplica client(transport, schema);
client.Poll();                       // once per tick: the only thing that reads the transport
client.State();                      // Connecting, Handshaking, Connected, Rejected or Disconnected
client.WordsOf(entity);              // the newest quantized state of a visible entity, or nullptr
client.ForEachEntity([](EntityId, const uint32_t* words) { /* ... */ });
client.Disconnect();
```

The client connects by itself: it sends its hello when the transport opens the connection, applies what the server
sends and confirms it. It does the following, and you do nothing:

- **Events in order.** Each `EventBatch` must be the next number. The reliable channel never repeats or skips one,
  so a repeat or a gap is a protocol error and ends the connection. Events that name an entity or a compact id that
  cannot exist end it too.
- **State only after the events it depends on.** A `StateBuffer` says how many event batches the server had sent
  when it wrote it. If the client has applied fewer, it keeps the buffer (up to 64) and applies it when the
  batches arrive; the oldest is dropped when too many wait. This is what stops state from being copied onto the
  wrong entity when a state buffer overtakes the events of its tick.
- **Whole or nothing.** A buffer the receiver refuses (late, from before the entity entered, or written against a
  state the client does not have) is counted in `buffers_refused` and not confirmed. The next buffer fills the gap.
- **One ack per `Poll`.** The newest applied tick and a mask of the 32 before it, so a lost ack is covered by the
  next one. The server remembers 64 ticks and confirms each tick once.

`ClientReplica::Stats()` counts buffers applied, refused, held and dropped, protocol errors, acks sent, malformed
and ignored messages.

A server hello that cannot work (a protocol version that is not the client's, more than 262,144 compact ids,
a baseline age outside 1 to 255 ticks or a tick rate outside 1 to 1,000) is a protocol error and ends the
connection. When the server says no, the client ends in `Rejected`, with the reason in `Rejection()`.

## What it costs

Measured numbers, with the machine and the code that produced them, are on the
[benchmarks page](../benchmarks.md#7-the-whole-network-path). In a world of 200,000 entities with 128 players who
each see about 1,750 of them, over a network that loses 5% of what it carries and delays every message by one to
two ticks, with a sixth of the entities changing in every tick:

- **A tick of the Network phase takes about 50 ms on one thread and about 14 ms with eight workers.** The goals
  are 3.3 ms and 1 ms. **Neither is met.** Most of the time is not in writing records (the server now writes about
  8 times fewer of them than before and the time barely moved): it is relevancy, the protocol and the transport,
  and this scenario moves 33,000 of 200,000 entities in every tick, about 48 times as many as the relevancy
  benchmark moves (700).
- **The server sends each player 98 kbps on average and 173 kbps at the busiest 10 seconds.** The goals are 100 and
  200, and they hold: a player who has been connected a while cannot be sent more than the budget. A new player
  gets a quick start from the saved-up budget, so the first seconds are above the average.
- **The price is how current the player's copy is.** With a sixth of the world changing in every tick, the changes
  are worth about 280 kbps per player and the budget carries 100, so each entity reaches the player about a third
  as often as it changes. At any moment 70% of the entities a player sees are not in the state the server holds
  (31% without a budget, where only the network's delay and the waits make a copy old). When the world stops, a
  player catches up in about two seconds (68 ticks).
- **Sending again is paced.** The server writes about 1.1 records for each change instead of 3.3, because it waits
  for the confirmation of an entity (the player's measured round trip plus a tick) before sending it again.
- Everything converges. Every player ends up with exactly what the server has once the world stops changing.
  A player who joins a running world gets everything it sees in about 61 ticks. Server and clients run the same
  with one worker or eight.
- A player costs about 1.8 MB in memory on the server and its own client together.

What lowers the time is next: making relevancy and replication faster. What would make the budget feel better is
sending what is near the player more often than what is far, which is not built yet.

## Known limits

- One server process, one grid, one set of tables with a single shard.
- The simulated network is an approximation (see above), and nothing here has met a real network yet.
- A forged but well-formed message is accepted (see "What the contract does not cover").
- A large message is lost far more often than a small one, which is why a state buffer is limited to 1,100 bytes
  by default: a player who joins a busy area gets its first state over several buffers instead of one.
- The wait before a resend has a resolution of one tick, and it follows an average, so a player whose connection
  suddenly gets much slower is sent some entities again too soon for a few seconds.
- A buffer is written against the state the player last confirmed. A player who stops confirming for longer than
  `max_baseline_age_ticks` is sent entities in full, which costs more of its budget.
- A player who reconnects starts with a full bucket, like a new one. The budget limits what a player is sent on one
  connection, not what an address is sent over several.
- The priority knows how long an entity has waited and the weight of its type, not how far it is from the player.
  Sending what is near more often than what is far is not built yet.

## Coming next

A transport over real sockets (so far only the simulated one exists), a dedicated server program, the Godot
extension that exposes the client, and priority by distance.

## Example

A server, one entity, one player and a lossy network, in a loop of ticks.

```cpp
#include <cstdint>
#include <span>

#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/transport/simulated_network.hpp"

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

// 3. The world: a table for the entities and a grid that knows where they are.
EntityTable<MobColumns> table(TableConfig{0, 1});
SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});

// 4. Players see entities of type 1 up to 150 m away (and keep them until 160 m).
RelevancyConfig relevancy;
relevancy.types[1] = TypeRelevancy{RelevancyMode::Spatial, 150.0f, 10.0f};

// 5. A network that loses 5% of what it carries and delays every message by up to two ticks. A real game
//    gives the server a transport that talks to real sockets instead; the rest stays the same.
NetworkConditions conditions;
conditions.loss = 0.05;
conditions.max_delay_ticks = 2;
SimulatedNetwork network(conditions);

// 6. The server. It owns the relevancy and replication engines and does the whole Network phase.
ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
RelevancyGroups groups;
ServerConfig config;
config.max_connections = 8;
config.max_visible_per_connection = 1024;
ReplicationServer server(config, relevancy, schema, grid, groups, jobs, network.Server());

// 7. Tell the relevancy engine where each player is looking from, as soon as the player is connected.
Observer eye{0.0f, 0.0f, 0.0f};
server.SetCallbacks(
    [&](ConnectionId id) { server.Relevancy().SetObservers(id, std::span<const Observer>(&eye, 1)); },
    [](ConnectionId) {});

// 8. Create an entity, write its columns, flag it as changed and tell the grid where it is.
const EntityId mob = table.Create();
const auto where = table.Find(mob);
MobColumns& columns = table.Columns(where->page);
columns.x[where->slot] = 120.5f;
columns.y[where->slot] = 0.0f;
columns.z[where->slot] = -40.25f;
columns.yaw[where->slot] = 1.0f;
columns.health[where->slot] = 80;
table.MarkDirty(mob);
grid.Insert(mob, 120.5f, 0.0f, -40.25f);

// 9. A player: a client on the other end of the network. It connects and says hello by itself.
ClientReplica client(*network.AddClient(), schema);

// 10. Every tick: the network delivers what is due, the client reads what arrived, and the server runs
// its
//     Network phase (quantize what changed, then update). Both sides confirm and resend by themselves.
for (uint64_t tick = 0; tick < 60; ++tick) {
    network.Advance(tick);
    client.Poll();
    server.Refresh(table);
    server.Update(tick);
}

const uint32_t* quantized = client.WordsOf(mob); // the position, angle and health as whole numbers
TESSERA_CHECK(client.State() == ClientState::Connected);
TESSERA_CHECK(client.VisibleCount() == 1);
TESSERA_CHECK(quantized != nullptr &&
              quantized[0] == 512'050); // x = 120.5 m is 512,050 steps from -5,000 m
```
