# 0006. Relevancy by entity type, with hysteresis and groups

- **Status:** Accepted
- **Date:** 2026-11-06

## Context
The server must decide, for every player and every tick, which entities that player needs. Doing it per
player and per entity every tick is far too expensive for a world with hundreds of thousands of entities.
The rules also have to be easy to understand: a developer should be able to say "crates are visible from
150 m, the world clock is visible to everyone" and have it work.

## Decision
- **Reach belongs to the type of entity, not to the player.** Each type has a mode (spatial, always
  relevant, owner only) and, for spatial types, a range. A large thing can be seen from far away and a small
  one only from close by.
- **Ask the grid once, then filter exactly.** For each observer the engine asks the spatial grid for every
  entity within the widest range of any type, then keeps each entity only if it is inside the range of its
  own type, measured by exact distance.
- **Re-check every few ticks, not every tick.** Each player's visible set is recomputed every N ticks
  (3 by default), staggered between players so the work is even. Destruction is the exception: it is
  reported at once. N and the margin below are configuration.
- **A hysteresis margin per type.** An entity that is already visible stays visible a little beyond its
  range, so entities at the edge do not flicker.
- **Groups for exceptions.** An exclusion group hides its members from every player except those it allows;
  an inclusion group shows its members to the players it allows. A player sees what the type allows plus
  what inclusion adds, minus what exclusion hides, so exclusion always wins.
- **Several observers per player.** What a player sees is the union of what each of their observers sees
  (a camera, a drone, the map). The common case, one observer, pays nothing extra.
- **Compact ids per connection.** Each visible entity gets a short id on that connection. A freed id is not
  reused until the next tick.
- **Each player's work is independent.** During the update every player reads shared data and writes only to
  their own state, so players can be processed in parallel without locks. The result is the same with one
  worker or many.

Which entities have changed since they were last sent (dormancy) is not decided here. It belongs to the
layer that sends entity state.

## Consequences
- The rules are easy to explain and to configure, and a game can add exceptions without touching the engine.
- The cost of a re-check depends on how many entities lie within the widest range, so a type with a very
  large range makes every re-check more expensive. Choose the widest range carefully.
- The delay before an entity appears is up to N ticks (about 100 ms with the defaults at 30 Hz).
- Recomputing the whole visible set is simple and correct, but it is not the cheapest possible approach.
  See [0007](0007-relevancy-first-version.md).
