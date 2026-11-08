# 0010. Transport by messages, a protocol with numbered event batches, accepted at its measured cost

- **Status:** Accepted
- **Date:** 2026-11-07

## Context
With relevancy ([0006](0006-relevancy-by-type-and-groups.md), [0008](0008-incremental-relevancy.md)) and replicated
state ([0009](0009-replicated-state.md)) in place, the server has to put their output on a network and the player
has to apply it. Three things were open: where the line between our code and the network library goes, what the
messages look like, and how the player knows that a state buffer may be applied. The goals for the whole Network
phase with 128 players were 3.3 ms per tick on one thread, 1 ms with eight workers and 100 kbps per player.

## Decision
- **The transport moves whole messages.** `ITransport` sends and receives messages on two channels, reliable and
  ordered, and unreliable, and reports connections opening and closing. Cutting a large message into packets and
  putting it back together happens inside the transport. The alternative was to make our code cut buffers to the
  size of a packet, which every transport would then have to be built around. The cost is that a large message
  is lost far more often than a small one over an unreliable fragmenting transport, because every piece has to
  arrive; the consequence is that buffers must stay small, which is the job of the layer that decides what
  goes first.
- **A simulated network in memory, with a seed.** `SimulatedNetwork` loses, repeats and delays messages by a rule
  that depends on size, and delivers reliable messages in order. Tests and benchmarks run the real server and the
  real clients over it and get the same result every time. It is an approximation of a real transport, not a
  replacement for trying one.
- **Five messages.** Hello from each side, an event batch, a state buffer and an ack. The decoders are strict:
  a truncated message, leftover bytes or an impossible value are refused.
- **Visibility events travel reliably, numbered; state travels unreliably and says which events it needs.**
  Each event batch carries a number (1, 2, 3, ...) and each state buffer carries how many batches the server had
  sent when it wrote it. The player applies batches in order and keeps a state buffer until it has applied that
  many. The alternative was to number state buffers and events on one stream, which would make state wait behind
  every lost packet. With the count, state is never applied before the events it depends on and a reliable
  channel does not hold anything back. A repeat or a gap in the numbers can only be the server's fault, so it
  ends the connection.
- **One ack per tick, with a mask.** The player sends the newest tick it applied and a mask of the 32 before it.
  The server remembers 64 ticks and confirms each once. Only buffers applied whole are confirmed.
- **Send nothing when there is nothing to say.** No event batch without events, no state buffer without a
  record: a player whose world does not change costs no bytes.
- **The server owns both engines.** `ReplicationServer` runs the whole Network phase in one call, so that the
  order (events before state, confirmations before relevancy) cannot be got wrong by the caller. It overrides
  the connection and visible-entity limits of the relevancy configuration with its own.
- **Connections do not trust the wire to be honest.** A reliable message that cannot be read, or one a player
  never sends, disconnects that player; the same on the unreliable channel is dropped and counted. An ack for a
  tick the server has not sent is dropped too. Other forged messages that are well formed cannot be told from
  real ones; checking packets is left to the transport. The server also refuses, at construction, a type that
  players can see but that has no replication schema.
- **Left out on purpose:** a transport over real sockets, encryption and authentication, reconnecting into an
  old session, and pacing of repeats (done afterwards, see [0011](0011-priority-and-bandwidth-budget.md)).

## Consequences
- The server and the client now run together, in C++ and without Godot, and the end-to-end tests prove that
  every player converges to what the server has under loss, duplication, delay and reordering, reconnection,
  128 players, garbage input, and one worker or eight.
- **The goals for the whole phase are not met.** With 128 players in a world of 200,000 entities and a sixth of
  them changing in every tick, the phase takes about 66 ms per tick on one thread and about 21 ms with eight
  workers, and the server sends about 960 kbps to each player. The regression limits in the benchmark are set at about twice
  these values. The numbers, the scenario and the machine are on the
  [benchmarks page](../benchmarks.md#7-the-whole-network-path).
- Most of the bytes are repeats: a changed entity is sent again every tick until its confirmation returns,
  which writes about three times more records than there are changes. Deciding how often to repeat and what
  goes first when there is too much to send was the next piece of work. It is [record 0011](0011-priority-and-bandwidth-budget.md),
  and it brought the bytes within the goal (the numbers above are from before it).
- The ack mask carries no information until repeats are paced, because everything not confirmed is sent again
  in every tick anyway.
- A transport over real sockets has to meet the contract in the
  [networking reference](../reference/networking.md#the-transport); nothing above it changes.
