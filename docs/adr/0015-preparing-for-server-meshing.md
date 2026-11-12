# 0015. Preparing for server meshing

- **Status:** Proposed
- **Date:** 2026-11-12

## Context
Server meshing means several server processes sharing one world. Nothing here builds it, and nothing here says it will be
built. The reason to write this down now is that the API is about to be frozen, and a choice made for a single process
can close that door without anyone noticing.

Two pieces already look ahead. The entity id has 12 bits for a shard ([0005](0005-entity-identification.md)), and the
server has a `shard_id` setting that reaches those bits ([0014](0014-the-server-as-a-library-plus-an-example.md)). We
wanted to know whether the rest of the code agrees with them, so we read the core and the server with one question: what
would stop two or more servers from sharing a world? The answer is a short list of rules to keep, a list of limits we
found, and a list of things we have not decided.

## Decision
These rules apply to every change from here on. Each one can be answered with yes or no by looking at the code, and each
says how we checked it today.

1. **Only an `EntityTable` makes entity ids, and it stamps them with the shard of the process that owns it.** Checked by
   searching for `EntityId::Make`: it is called only inside `EntityTable`, with the shard from its `TableConfig`. The test
   `host_entities_carry_the_shard_id_to_the_client` shows the setting reaching the ids, and
   `config_rejects_shard_id_4096_and_accepts_4095` shows the range.
2. **`ConnectionId` and `TransportConnection` mean something only inside the process that handed them out.** Checked by
   reading `protocol.hpp` and its users: `ServerHello::connection_id` is kept by the client and never sent back, and no
   other message carries either type.
3. **A tick is compared only with ticks that the same server sent on the same connection.** Checked by reading every use
   of `now_tick_` and `latest_tick`: the round trip is the difference between the server's own tick and one it sent to
   that peer, and an acknowledgement is accepted only for a tick that was sent to that peer.
4. **No mutable global state holds world or game data in `core/` or `server/`.** Checked by searching for non-constant
   `static`, `thread_local` and file-scope variables. Three things turn up and none of them holds game data: the count of
   live ENet transports, under a mutex; a per-thread timer in the tick loop (Windows only); and the counters that exist
   only in a build made with `TESSERA_PROFILE`. The example program also keeps its stop flag, and that is not in the
   library.
5. **A `ServerHost` owns everything its server uses, so two hosts in one process share nothing but a logger.** Checked by
   the test `host_two_hosts_in_one_process_do_not_share_state`.
6. **A spatial grid belongs to one relevancy engine, and an engine to one replication server.** Checked by reading the
   headers of `SpatialGrid` and `RelevancyEngine`, which say so, and by the way `ServerHost` builds one of each.

### What we found
We went through ids, relevancy, replication, transport, time and the production server. The rules above come from the
points that held. The rest are limits of today's code, and none of them is fixed here.

- **An entity cannot move to another process and keep its id.** An `EntityTable` picks the slot and the generation, and
  the replication side only reads entities from a table. The id format allows the move, as 0005 says, and the code does
  not. Giving an entity a new id when it crosses over is possible today and costs the game some remapping. Closing this
  gap would change public types, so it was not attempted.
- **The shard is not an owner.** Nothing records which process is in charge of an entity. The shard is part of the
  identity, so using it as an owner would force an id change whenever the owner changes. The `SetOwner` of the relevancy
  engine names a player's connection, not a process.
- **Two shards can look alike inside the replicated state.** The store of quantized values is indexed by type and slot,
  without the shard, so entities from two shards with the same type and slot would share an entry.
- **Relevancy only sees its own grid.** An entity from another process that is put on the grid is announced to the player
  and never gets any state, because the state store is fed only by local tables.
- **A player is a connection, not an identity.** `ClientHello` does not say who is connecting, so a server cannot tell a
  player who arrives from another server.
- **Time is local.** The tick counter starts at zero in each process and drifts from the clock after a tick that
  overruns, and the game callback is not told the tick number. A client joined to two servers has no common timeline.
- **Smaller points.** `shard_id` is checked for range only and appears in one log line. The signal handling of the example
  program notifies only the last host. A client transport talks to one server.

What we checked and found not in the way: per-connection state is indexed by local ids and shares nothing across
processes, acknowledgements and baselines are per connection, the schema hash carries no process data, and the transport
interface is the same on both ends.

### What we have not decided
- A protocol between shards, and whether processes talk to each other at all.
- Moving entities between processes, and what stays the same for the client when it happens.
- World borders, and who is in charge of an entity that stands on one.
- Load balancing.
- What a client connected to several shards sees, and whether it connects to them or to one that relays.
- How time is kept in step between processes.
- Whether an entity gets an explicit owner, and what that would look like.
- Whether any of the limits above should be removed, and in which order.

## Consequences
- A change to the public API or to a message is read against the six rules. That costs a review question and little else:
  the checks are a search, a read of a few files and tests that already exist.
- The limits stay in the code until someone decides they matter. The list above describes today's code and is not a
  commitment to remove them.
- We gain no guarantee that server meshing will come, and no date. The rules only keep us from closing the door by
  accident.
- If a later record settles any of the open questions, it replaces the matching lines here.
