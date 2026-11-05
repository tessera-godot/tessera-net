# 0005. Entity identification

- **Status:** Accepted
- **Date:** 2026-11-05

## Context
An entity id is used in three places: inside one server process, in storage, and (later) on the network
and between servers. On the network every update carries an id, and an 8-byte id would by itself exceed
the size we want for a whole position update.

## Decision
- **Server-side id (`EntityId`):** 64 bits. Shard 12 | type 8 | index 28 | generation 16. It is globally
  unique, so entities can later move between servers without changing identity. Generations 0 to 65,534
  are valid; 65,535 is a reserved "retired" marker and the null id has all bits set, so it can never be
  valid.
- **Wire id:** each connection gets a short id (about 16 to 20 bits) for the entities it can see. The
  mapping is sent once, when an entity enters a player's view, and the short id is reused when the
  entity leaves. Updates then carry only the short id. This part belongs to the replication layer and is
  not implemented yet.
- **Generation exhaustion:** when a slot's generation would reach the retired marker, the slot is
  retired permanently. The memory cost is negligible and no old id can ever match a new occupant.
- **Order keys:** commands queued by many threads carry an explicit order key and are applied in key
  order, so structural changes are identical with any number of workers.

## Consequences
- Stale ids are detected cheaply and safely.
- The id already contains the shard, which keeps the design open for a multi-server world.
- The replication layer must keep a translation table per connection (budgeted at a few megabytes).
