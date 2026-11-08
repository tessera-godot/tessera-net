# 0011. Resends by measured round trip, priority by waiting time and weight, and a byte budget for each player

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
After [0010](0010-transport-and-protocol.md) the server sent a changed entity again in every tick until its
confirmation came back, and it wrote as many records as fitted its idea of the tick, in one buffer of any size. With
128 players in a world of 200,000 entities and a sixth of them changing in every tick, that was about 960 kbps per
player against a goal of 100, in buffers of tens of packets that a lossy network almost never delivered whole.
Three things were open: how long to wait before sending an entity again, what goes first when there is not room
for everything, and how to keep a player's bytes within a promise.

## Decision
- **An entity that was sent and not confirmed waits for the player's measured round trip.** The server takes the
  number of ticks between writing a buffer and the first ack that confirms its newest tick, and averages it (each
  new measurement counts for an eighth). The wait is that average rounded up, plus a margin of one tick, between
  a minimum of 2 and a maximum of 30; until the first measurement it is 6. A fixed wait is wrong for most players,
  too short for a slow one (it wastes the bytes the budget gives) and too long for a fast one (it delays a lost
  update). An entity that changed is never made to wait.
- **A byte budget is a bucket for each player.** It fills at the rate (100 kbps by default) and holds the saved-up
  difference between the peak (200 kbps over 10 seconds by default) and the rate, and it starts full. Every buffer
  takes its size and 12 bytes for the headers out of it. The event batches are taken out after they are sent, so
  the bucket may go into debt, and the player then waits for the debt to be paid. We did not make the batches wait
  for the budget because they carry visibility events, and a state buffer is only applied after the events it
  depends on; holding them would hold the state.
- **A buffer has a hard size limit** (1,100 bytes by default, one packet). The limit is exact: the writer takes a
  record back out if it would cross it. The alternative, an estimate with a margin, would sometimes send more than
  promised and sometimes less.
- **What goes first is decided by how long an entity has been ready, times the weight of its type.** The score is
  the waiting ticks plus one, times the weight (1 to 255, 1 by default). Ties go to the lower compact id. An entity
  that does not fit is skipped, and a smaller one after it may still fit, because otherwise a large record could
  hold up a buffer of small ones. We chose age over distance for the first version because it is enough to
  guarantee that nothing waits forever, and distance needs a decision about what "near" means for each type.
- **Nothing is dropped.** An entity that did not fit is a candidate again in the next tick, with a larger score.

## Consequences
- **The goals for the bytes are met, by construction.** In the network benchmark a player gets about 98 kbps on
  average (goal 100) and 172 kbps at the busiest 10 seconds (goal 200), and the server writes 1.09 records per
  change instead of 3.3 when the budget is off. The numbers are on the
  [benchmarks page](../benchmarks.md#7-the-whole-network-path).
- **The price is how current a player's copy is.** The changes of that world are worth about 280 kbps per player
  and the budget is 100, so an entity reaches a player about a third as often as it changes, and at any moment 67%
  of what a player sees is not the current state (31% without the budget). A player catches up in about two seconds
  after the world stops changing. We considered guarding this measure with a limit of 50% and decided against it: it
  could only be met by raising the budget, which is a different promise. The measure is reported and not guarded.
- **The time of the phase barely changed** (about 50 ms per tick on one thread against a goal of 3.3), because
  writing records was not where most of the time went. The next place to look is relevancy with many moving
  entities, with a profiler.
- Buffers are one packet, so a lost message loses one packet's worth. A new player's first state arrives over
  several buffers: everything it sees in about 53 ticks in a running world.
- The wait has a resolution of one tick and follows an average, so a player whose round trip suddenly grows is sent
  some entities again too soon for a few seconds. A player who reconnects starts with a full bucket, as a new one
  does. Sending what is near more often than what is far is not built yet.
