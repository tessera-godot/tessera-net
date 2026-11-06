# 0007. The first relevancy version is accepted at its measured cost

- **Status:** Superseded by [0008](0008-incremental-relevancy.md)
- **Date:** 2026-11-06

## Context
The relevancy engine ([0006](0006-relevancy-by-type-and-groups.md)) had cost goals for the largest scenario we
aim for: 128 players and one million entities in a 10 km by 10 km world, 700 entities moving per tick.
The goals were 1.5 ms per tick on one thread and 0.4 ms with eight workers. The first is already 45% of the
3.3 ms we allow, on one thread, for the whole network phase.

The first measurement, taken with the first version of the benchmark (it is in the git history), was far from
them: 136 to 147 ms on one thread and 23 to 24 ms with eight workers. The cause was visible in the numbers. A query to the spatial grid returned about 11,600 entities and took
161 ns for each, because the grid looked every entity up in a hash table. Two changes fixed most of that
without changing behaviour, and the tests that compare the engine with a deliberately simple model confirm it:

- Each grid cell now stores the position next to the entity id, so a query reads memory in order.
- The engine only consults a player's list of visible entities for entities in the hysteresis band, not for
  every candidate.

With them the benchmark measures about 13.6 ms on one thread and about 2.3 ms with eight workers (medians of
four runs; full numbers and the machine are on the [benchmarks page](../benchmarks.md#5-relevancy)). A re-check
still costs about 320 µs per player, and the goal corresponds to about 35 µs.

## Decision
- Accept this version as the first working relevancy engine and publish its real numbers.
- The limits in the benchmark become **regression guards** at 25 ms (one thread) and 5 ms (eight workers):
  they fail the benchmark if the engine gets slower than it is now, with room for run-to-run variation.
  They are not the goals.
- The goals of 1.5 ms and 0.4 ms stay as the target for the next version, which re-evaluates only what
  changed (entities that moved and the edges of each observer's view) instead of recomputing the whole set.

We considered keeping a separate list per entity type in every cell, so each type is scanned only within its
own range. It would reduce the candidates (we have not measured by how much), but not the cost of comparing and
sorting the visible set, which we estimate is now the larger part of a re-check (we have not timed it
separately), so we did not do it.

## Consequences
- Servers with fewer players or sparser worlds fit today, with several workers: in the smaller benchmark
  scenario (32 players, 250,000 entities, the same density) a tick costs about 3 ms on one thread, which uses
  most of the 3.3 ms budget, and 0.6 ms with eight workers.
- The largest scenario does not fit its network-phase budget yet. On one thread the engine alone takes about
  four times the 3.3 ms; with eight workers it takes about 70% of it.
- The visible set is a hash map that allocates a node for every entity that enters a player's view. The next
  version should replace it with a structure that does not allocate per entity.
- Entity count by itself barely matters; the number of players and how many entities lie within the widest
  range do.
- The next relevancy version will need its own design record.
