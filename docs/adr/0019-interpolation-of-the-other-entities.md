# 0019. Interpolation of the other entities

- **Status:** Proposed
- **Date:** 2026-11-13

## Context
After [0018](0018-prediction-and-reconciliation.md) the player's own sphere moves at once, but everything else the client
shows, the other players and the entities that walk by themselves, is the last state received: it jumps from one state
to the next. The server sends the state of an entity at most 30 times a second, and less often for an entity that waits
behind the budget of bytes, so a sphere that moves at 8 meters a second advances in steps of 27 centimeters, or more.

The client has to show those entities moving smoothly, with nothing new in the protocol.

## Decision
**Interpolation with a fixed delay, and no extrapolation.** The client shows each entity a few ticks in the past (the
delay, 2 ticks by default, about 66 milliseconds at 30 ticks a second) and moves it along the line between the two
states it has received on either side of that moment. The alternative, extrapolation, guesses where the entity is going
from where it has been: an entity that changes direction is shown going where it no longer goes, and one that stops is
shown walking on until the state that says so arrives. The cost of interpolation is the delay, and one more: a state
that arrives later than the delay finds the entity waiting where it was, and it moves on when the state arrives.
Nothing is shown that has not been received.

**It lives in the core, inside the client.** `Interpolator` is a class of its own, as `Predictor` is, that `ClientReplica`
owns and feeds; the Godot extension only exposes it. It is fed by the same call that stores the state: `ReceiverState`
tells an observer about each record it stores, so the interpolator holds what the client holds, late states included, and
there is no second path for the data to take that could disagree with the first. When interpolation is off the observer
is not passed and nothing is held. No message changed, and the protocol stays at version 3.

**A history for each entity, in a pool for each type.** An entity keeps its newest `delay + 6` states in order of tick
(8 with the default delay; at most 36): 2 for the slack of the clock, 4 for how far the clock may fall behind before it
jumps, and one more to close the segment being shown. The states live in a pool for each entity type, in blocks of 128
entities, taken when the entity becomes visible and given back when it leaves, so a state that arrives or a frame that is
drawn allocates nothing. A state that arrives out of order is put in its place, a repeat of a tick is ignored, and when the
history is full the oldest leaves (a state older than all of them is ignored). The numbers are counted
(`InterpolationStats`).

**What is interpolated.** A position is blended linearly in `double`, on the values the words stand for (blending the
words themselves would round every frame to a step of the schema, and at 60 frames a second a movement of 8 meters a
second is 4 millimeters a frame, which the rounding would turn into a stutter). An angle goes the short way round, so 350
degrees to 10 goes through 0 and not through 180. An integer, an enum and a set of flags never take a value in between:
they change all at once, at the tick of the state that brings them, which is when the position that goes with them
changes, so what is shown is a picture of one moment.

**The clock.** The time shown is a number of ticks, a `double`, kept by the interpolator, which reads no clock: whoever
calls it says how much time passed (`AdvanceInterpolation(seconds)`; the extension's `poll` does it with the time since
the last call, at most a quarter of a second). It is aimed at the newest tick received minus the delay. Within 2 ticks of
that aim it runs at the speed of time; further ahead it runs a tenth slower and further behind a tenth faster; if it
falls more than 4 ticks behind (the network stopped and came back, or a much newer tick arrived) it jumps to the aim
instead of replaying what was missed; and it never passes the newest tick, which is why nothing is extrapolated. A
frame that took a quarter of a second cannot take it past the newest state. Because the correction starts only when the
clock is more than 2 ticks ahead of its aim, the time shown can sit up to 3 ticks closer to the newest tick than the delay
says; it is never further than the delay.

**A long gap, and a jump, are not slid through.** The client cannot tell an entity that stood still for three seconds
(the server sends nothing about it) from one that the budget of bytes kept waiting; it only sees two states far apart. If
two neighbouring states are more than `max_gap_ticks` (8) ticks apart, or more than `snap_distance` (2.5 world units, all
the positions of the type together) apart, they are not blended: the entity stays at the older one and goes to the newer
at the tick of the newer. A teleport does not slide, and an entity that stood still for a long time and then moved does
not walk the whole silence. The decision is made when the state arrives and kept with it, so reading a value decides
nothing. The numbers are chosen so that the fastest thing in the example, a player at 8 meters a second, moves 2.13
meters in 8 ticks, which is under 2.5: a game with faster entities has to keep `snap_distance` above its fastest speed
times `max_gap_ticks` over the tick rate. The extension uses 8 ticks; `max_gap_ticks` is a setting of the core only.

**The entity the player predicts is not interpolated.** For it, `get_interpolated_property` gives what
`get_predicted_property` gives, and when the prediction is not running (waiting, paused or failed) the last state of the
server: showing the player's own sphere 66 milliseconds behind the guess it is steering would undo the prediction. A
player with no prediction set is an entity like the others, and is interpolated.

**What the game has to do.** Call `set_interpolation` once (before connecting is fine, and it stays through a
reconnection), call `poll` every frame, and read `get_interpolated_property` where it drew `get_property`. `get_property`
still gives the last value received. None of the calls goes through the guard of 0018: they do not call back into the
game, so a prediction rule may use them.

**What it costs.** Measured in a build with optimization (MSVC, `bench_interpolation`), with 1,000 and 10,000 entities of
5 words (a position and a 64-bit number), a delay of 2 ticks:

| | 1,000 entities | 10,000 entities | Limit for 10,000 |
|---|---|---|---|
| Take in one tick of states | 26 us (26 ns each) | 270 to 285 us (27 to 29 ns each) | 1,000 us |
| Read every position | 11 us (11 ns each) | 113 to 120 us (11 to 12 ns each) | 500 us |
| Advance the clock | 3.5 ns | 3.5 ns | 10,000 ns |
| Memory for each entity | 270 B | 256 B | 384 B |

Advancing the clock does not depend on the number of entities. An entity of 3 words takes 190 to 205 bytes, and at the
longest delay (30 ticks, 36 states) an entity of 5 words takes 1.1 KB. For comparison, the receiver that the client
already has keeps up to 33 states of 136 bytes for an entity that changes every tick, about 5.9 KB: interpolation adds
4% to what the client holds for the same entities. It is off unless set, and off it costs nothing. If it ever costs too
much, the next step is to interpolate only some types, or only what is near; that is not built.

**What the simulated network showed.** Against the example server, with a player at 8 meters a second and delays
between 0 and 2 ticks, with and without 30% loss and 10% duplication, on three seeds each, the shown position stayed
between positions that had been received, never went back by more than a step of the schema, and never passed the
newest state; with no loss it was on the true path of the player to within a step of the schema. The longest gap that was
still blended was 7 ticks and the longest move between two blended states 1.87 meters, under the 2.13 that the limit
allows; no state was held for its distance, and at most one in 200 for its gap, which was the start of a movement after
a silence. Under a budget of bytes that carries half of what 200 walking entities need, the states of an entity are 2
ticks apart in the middle and 3 at the 99th percentile, 5 at most, all under the limit of 8; at a quarter of what they
need the middle is 4 ticks, the 99th percentile 5, the most 9, and about 1 state in 80 is more than 8 ticks after the one
before it, so those entities step instead of sliding. A game that sets a budget so tight needs a larger limit of gap,
which a game cannot set yet.

**Left out on purpose:** extrapolation, a delay that follows the jitter, cubic or velocity-based interpolation, lag
compensation or rewinding on the server (the other players are shown 66 milliseconds in the past, and a player aiming at
one aims at where it was), interpolating the predicted entity, interpolation by level of detail (entities far away with
no history), a delay for each type of entity, and a gap limit and a snap distance that each game can set for each type.

**What was checked, and what was not.** The history, the clock, the values, the client and its use against the example
server on a simulated network that loses and delays messages are covered by tests of the core and of the server. They
were run on Windows (MSVC) on the machine where this was written, and on Linux in a container with GCC 13 and Clang 18,
in Debug and Release, with warnings treated as errors. The Godot calls are covered by a test that runs a Godot without a
window against the example server, on Windows and on Linux in a container with a Godot built from source; the continuous
integration does not run Godot, and has not run this change yet. The benchmark above was measured on Windows; in the
container it took 42 to 48 ns for each state and 16 to 18 ns for each position at 1,000 and 10,000 entities, still under
the limits. Nobody has looked at the demo with a window, so how smooth it looks is not judged.

## Consequences
**Easier:** the other entities move smoothly with one call, without a message of their own, and a state that is late or
lost makes an entity wait, not guess.

**Harder:** the other entities are shown 66 milliseconds in the past while the player's own is in the present, so what
the player touches or chases is a little behind where it is. The game has to keep `snap_distance` above how far its
fastest entity goes in a gap. Every visible entity takes a few hundred bytes more while interpolation is on.
