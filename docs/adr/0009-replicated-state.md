# 0009. Replicated state against the last confirmed state, accepted at its measured cost

- **Status:** Accepted
- **Date:** 2026-11-06

## Context
Once the server knows which entities a player sees ([0006](0006-relevancy-by-type-and-groups.md),
[0008](0008-incremental-relevancy.md)) it has to turn their state into bytes: small, cheap to produce for
128 players at once, and correct over a network that loses, repeats and delays packets. The goals for the largest
scenario we aim for (128 players, 1,500 visible entities each, about five updates a second per entity) were
10 million updates per second per core, at most 8 bytes for a position update, at most 128 bytes of state per
entity and 4 MB per player, nothing at all for an entity that does not change, and 70% efficiency with eight
workers.

## Decision
- **Write against what the player confirmed.** The server remembers, per player and per entity, the last state
  the player confirmed receiving, and writes only what differs from it. A lost packet never leaves the player
  out of step, because the next buffer does not depend on it. The other options were to keep the last several
  states of every entity and send each player a tick number (it costs memory for the whole world, not only for
  what each player sees) and to send the difference from the previous tick to everyone (a lost packet corrupts
  state until a full resend).
- **Quantize once.** Each changed entity is turned into small whole numbers once per tick, whatever the number of
  players that see it. Two states are equal when their numbers are equal, so comparing never touches a `float`.
- **Describe properties in a table, at run time.** Each entity type has a list of descriptors (which table
  column, which kind, how precise). The same table can drive the server and a client, and the code that will
  generate the schema only has to write this table. Loops inside are specialised by kind.
- **A record says which confirmed state it is written against.** A difference of positions can only be read
  against the very state it was computed from, and a player may already have applied a newer buffer whose
  confirmation was lost. So a partial record carries the age of its baseline, and the receiver keeps the states
  of the last 32 ticks of each entity. An older confirmed state is not used: the entity is sent in full.
- **Remember the recent sends.** An entity that is sent again before its first confirmation arrives has to
  keep every send, or no confirmation would ever match the latest one and nothing would be confirmed while the
  round trip is longer than a tick. Each entity keeps its last 8 different sends per player (the same state sent on
  consecutive ticks counts once). Confirming a tick promotes the state sent then. When every later send carried
  the confirmed state and nothing changed since, the entity stops. Comparing only the newest send was not enough:
  after A, B, A the player may hold B.
- **The receiver must know how far the events have got.** A buffer that arrives before the visibility events of
  its tick can name compact ids that already mean other entities on the server, and with entities of the same
  type that would copy one entity's state onto another without any error. The receiver is told, with each Enter
  event, the tick it was produced in, and with `AdvanceEvents` how far events have been applied. It refuses a
  buffer for a tick ahead of that (`EventsMissing`) and one from before an entity entered (`StaleRecord`).
- **Mark only who sees the entity.** An index from each entity to the players that see it and the compact id each
  has is kept from the visibility events. When an entity changes, only those players are marked. Without it every
  player searched its visible set for every changed entity, which was almost all of the time.
- **Leave room for more kinds of property.** Only positions, angles, integers, bit masks and enums are built. The
  format has room for the rest (vectors, quaternions, strings, arrays, nested structures) as new codecs.

## Consequences
- With 128 players seeing 1,500 of 200,000 entities each, a position update takes about 4.4 bytes, an entity
  about 54 bytes of stored state, a player about 0.7 MB, and a player who sees nothing that changed gets one
  byte per tick. Those goals are met.
- **Two goals are not.** On one thread the engine writes about 5.8 million updates per second (the goal is 10
  million), and with eight workers it reaches about 44% efficiency (the goal is 70%), because marking what
  changed runs on one thread. The benchmark limits are regression guards at about half of these values. The
  numbers and the machine are on the [benchmarks page](../benchmarks.md#6-replicated-state).
- About 173 nanoseconds in all go to each record on one thread (33 ms for 192,000 records, marking included),
  against 48 for building and writing one record alone with the data in cache. We tried fetching the states
  ahead of use and it changed nothing; we think the per-player state (about 0.7 MB each, 90 MB for 128
  players) does not fit in the processor cache, but we have not shown it with a profiler. An earlier
  measurement of the version before two defects were fixed gave 6.5 million; see the benchmarks page.
- An entity is sent again on every tick until a send of it is confirmed, which triples the records in the typical
  case (three ticks until the confirmation of a two-tick round trip). Waiting before a repeat brought the
  typical tick from about 22 ms to between 11 and 15 ms in an experiment we did not keep. How often to repeat
  belongs to the layer that decides what goes first and how much fits, which is the next piece to be built.
- If a player's round trip is longer than the remembered sends (8 ticks, about 270 ms at 30 Hz), an entity that
  changes on every tick is sent in full every time, because no confirmation matches. The setting can be raised for
  such players, at the cost of memory for each entity and player. An entity that does not change settles
  whatever the round trip.
- Whoever delivers visibility events to a player has to do it in order, with each event's tick, and call
  `AdvanceEvents`. Buffers may be lost, repeated or late.
- Making this faster is a piece of work of its own: measure with a profiler, shrink what is kept per player and
  entity, spread the marking over the workers, and space out the repeats.
