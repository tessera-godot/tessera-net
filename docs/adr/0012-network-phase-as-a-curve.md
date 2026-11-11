# 0012. The time of the network phase as a curve

- **Status:** Proposed
- **Date:** 2026-11-11

## Context
The goal for the network phase of a tick is 3.3 ms on one thread and 1 ms with eight workers for 128 players (see
[0007](0007-relevancy-first-version.md) and [0010](0010-transport-and-protocol.md)). It has been measured at one
point only, the hardest scenario we could build: 200,000 entities, a sixth of them (33,333) changing in every
tick, about 290 changes in view of each player. At that point
the phase takes 38.1 ms on one thread and 10.8 ms with eight workers in the measurement below (the benchmarks page
gave 31.6 and 8.7 ms for the same scenario before; see the conditions in the consequences).

One point does not say how the cost grows with the load, and that scenario is much heavier than most games. How many
entities exist says little about how many move: Epic's documentation for its
[Replication Graph](https://dev.epicgames.com/documentation/en-us/unreal-engine/replication-graph-in-unreal-engine)
says a Fortnite Battle Royale game starts with 100 players and about 50,000 replicated actors. A server pays for the
ones that change, not for the ones that exist, which is why we measure by entities moving. We needed the cost at
several loads before we could say what a goal for it means.

## Decision
- **The goals for the phase are written by entities moving per tick**, for 128 players and the reference world
  (200,000 entities in 4,500 m by 4,500 m, 5% packet loss, the default budget of 100 kbps).
- **3.3 ms and 1 ms stay as the long-term goal at the stress point**, 33,333 moving entities per tick.
- **Near term, the original goals apply to a reference load of 1,000 moving entities per tick**, and the heavier
  points have ceilings that say "do not get worse than today". The reference load is our estimate of a busy game; we
  have not checked it against real games.

| Entities moving per tick | One thread | Eight workers |
|---|---|---|
| 1,000 (reference load) | 3.3 ms | 1.0 ms |
| 2,985 | 7.0 ms | 2.5 ms |
| 10,000 | 17.0 ms | 5.0 ms |
| 33,333 (stress point) | 38.5 ms | 11.0 ms |
| 33,333, long term | 3.3 ms | 1.0 ms |

The values for 2,985, 10,000 and 33,333 are the measured medians below rounded up to the next half millisecond.
They are ceilings against getting worse, not aims. On this busy machine the slowest of the four executions already
goes over them (7.76 > 7.0, 17.67 > 17.0, 40.33 > 38.5 and 11.30 > 11.0 ms), and the median of the `network` mode,
40.1 ms, is also above 38.5.

The curve, from `tessera_net_bench network-curve`, in milliseconds per tick (median of 4 executions, range in
brackets):

| Entities moving per tick | One thread | Eight workers | Kbps per player | Records per player per tick | Not current |
|---|---|---|---|---|---|
| 1 | 1.19 (1.16 to 1.26) | 0.64 (0.63 to 0.66) | 7.29 | 0.96 | 0.02% |
| 1,000 | 3.51 (2.85 to 3.61) | 1.23 (1.21 to 1.38) | 40.13 | 10.32 | 0.87% |
| 2,985 | 6.90 (6.32 to 7.76) | 2.26 (2.09 to 2.43) | 102.15 | 28.94 | 2.57% |
| 10,000 | 16.98 (15.90 to 17.67) | 4.67 (4.47 to 4.93) | 118.42 | 95.48 | 8.66% |
| 33,333 | 38.08 (37.46 to 40.33) | 10.81 (10.30 to 11.30) | 98.08 | 96.92 | 70.16% |

The periods that give these loads (an entity changes every 200,000, 200, 67, 20 or 6 ticks) are the only thing that
differs from the main scenario; the last row is that scenario.

The curve is not a straight line. A least-squares line has a fixed cost of 3.16 ms and 1.08 us per moving entity on
one thread (R squared 0.98), and 1.11 ms and 0.29 us with eight workers (0.99); these are the medians of the four
fits, one per execution, not a line through the five medians. Either way, the residuals reach 3 ms on one thread, and
the fitted fixed cost is 2.7 times the 1.19 ms we measured with one entity moving. The line is a rough summary and
does not describe the shape. The shape is this: the cost per moving entity falls as the load grows. Between
consecutive points it is about 2.3 us up to 1,000 entities moving, 1.7 us up to 2,985, 1.4 us up to 10,000 and 0.9 us
up to 33,333 on one thread (0.59, 0.52, 0.34 and 0.26 us with eight workers). From about 10,000 on the records written
per player stop growing (95 to 97 per tick), and the share of a player's copy that is not current is 70% at the top.
At 10,000 the figure for the last five seconds is 118 kbps, above the 100 kbps average, which the server can reach for
a while using what a quiet connection saved up (the budget allows 200 kbps over any 10 seconds); this run does not
show whether it settles at 100.

## Consequences
- **The new wording loosens what we promised for heavy loads, and we say so.** The old goal was the same 3.3 ms
  at every load, which at the stress point is 11.5 times away on one thread and 10.8 times with eight workers. A
  single goal that far off tells us nothing about progress, and it hid that the server is close at light loads. At
  1,000 moving entities the phase takes 3.51 ms (1.06 times the goal) on one thread and 1.23 ms (1.2 times) with
  eight workers. The goals for 2,985 and above now only protect what we have. We keep 3.3 ms and 1 ms at the stress
  point as the horizon, and the stress point is still far from it.
- **What the curve suggests, without promising anything.** With almost nothing moving the server is already near
  its goals (1.19 and 0.64 ms), so the players, the base relevancy and the transport are not the problem. The cost
  grows with movement from 1,000 to 10,000 entities, while the records written per player still grow, and that is
  where gathering and testing candidates and choosing what fits the budget probably weigh most; a profile at those
  points will tell. That is where further work should look first.
- **The reference load is a guess.** 1,000 moving entities per tick is a load we chose, not one measured in a real
  game, and the table should be revisited when we have one.
- **The measurements were taken on a busy machine.** Programs of everyday work were running and could not be closed;
  the processor was at 45% to 100% load before the executions (80%, 100%, 88% and 45%), and the short test that the
  benchmark prints for this (writing one position record) took 45 to 66 ns against 47 when the machine is quiet. The
  stress point measured 31.6 ms (8.7 ms with eight workers) in the main table above, in an earlier session, and 38.1
  ms (10.8 ms) in this one, so the shape of the curve matters more than the absolute values, and the ceilings above
  would need a re-measure on a quiet machine before anyone relies on them.
- The numbers are from an in-memory simulator, one machine and one build, with the packet loss simulated. They come
  from `bench/bench_network.cpp` (the `network-curve` mode) and are on the [benchmarks
  page](../benchmarks.md#the-time-of-the-phase-by-entities-moving). At every point the bytes sent are the same with 1
  and with 8 workers, in all four executions.
- This record stays Proposed until the values in the table are chosen.
