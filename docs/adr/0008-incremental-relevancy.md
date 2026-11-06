# 0008. Relevancy is updated incrementally, with a small tolerance

- **Status:** Accepted
- **Date:** 2026-11-06

## Context
[0007](0007-relevancy-first-version.md) accepted a relevancy engine that rebuilt every player's visible set
every few ticks. It measured 15.4 ms per tick on one thread and 2.13 ms with eight workers in the main
scenario (128 players, one million entities, 700 moving per tick, players moving at walking and driving
speeds), against goals of 1.5 ms and 0.4 ms. Most of that work was repeated: between two looks around a
player, almost nothing near them has changed.

## Decision
- **Each observer has an anchor.** It is the position the engine last looked around it from. A full look is
  done again only when an observer is `observer_tolerance` (4 m by default) or more from its anchor, when
  the observers of a player change in number, or when something that does not depend on distance changes
  (an owner, a group, an `AlwaysRelevant` entity, a new connection, ids freed after running out).
- **Between full looks, only what changed is reconsidered.** The grid can record insertions, removals and
  moves (off by default; the relevancy engine turns it on and empties it every `Update`). For each player
  the engine takes the entities that changed near one of their anchors and judges them by where they are now.
  Changes are found through an index by grid cell, so a player does not have to read every change.
- **The visible set is a sorted list.** Applying a tick's leavers and enterers is a merge, and events come
  out in entity id order without a separate sort.
- **Queries go by type.** Every cell keeps its entities in one group per type, so a full look asks for each
  type only within that type's own reach instead of one query at the widest reach.
- **Full looks per tick are capped.** `max_reanchors_per_tick` (32 by default) limits them; the players that
  have waited longest go first, and the rest are served in the next ticks. This protects the tick when many
  players need a look at once.
- **The guarantee is stated in meters.** With tolerance T, an entity within `range` minus T of a player's real
  position is visible, and an entity beyond `range` plus the hysteresis margin plus T is not. We chose this over
  an exact answer at every tick, which costs about as much as the first version, and over a looser tolerance,
  which a player could see as entities appearing late.
- **The setting `recalc_interval_ticks` is removed.** It described the old schedule. Entities no longer wait
  for a re-check tick to appear.
- **One engine per grid.** The engine owns the grid's change record, so two engines cannot share a grid, and it
  takes the grid and the groups as non-constant references because it clears the record and removes destroyed
  entities and removed players from the groups.

## Consequences
- In the main scenario a tick costs about 1.03 ms on one thread and 0.33 ms with eight workers (medians of
  four executions; numbers, machine and code on the [benchmarks page](../benchmarks.md#5-relevancy)). Both
  goals are met. Compared with the first version under the same movement, that is 15 times faster on one
  thread and 6.5 times with eight workers.
- A player who moves fast needs a full look more often (every 5 ticks at 25 m/s with a 4 m tolerance). With half of
  the players driving, the main scenario takes about 2.5 ms on one thread and 0.57 ms with eight workers. A
  world where everyone drives all the time needs more workers or a larger tolerance.
- The engine adds memory per player for the anchors and the sorted visible set: about 0.16 MB per player in the
  benchmark.
- The result depends on the order of events only through entity ids, so it is the same with one worker or many.
  Tests compare the engine with a deliberately simple model that uses the anchors the engine reports.
- Dormancy (which entities have changed since they were last sent) is still decided by the layer that sends
  entity state, not here.
