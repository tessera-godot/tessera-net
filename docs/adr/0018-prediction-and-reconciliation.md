# 0018. Prediction and reconciliation

- **Status:** Proposed
- **Date:** 2026-11-13

## Context
After [0017](0017-player-input.md) the player's input reaches the server and the player's sphere moves, but only when
the server's answer comes back: one round trip after the key. That is too slow for something the player is steering. The
record also left the confirmation of inputs for later, because it would change a message that already existed.

The player's client has to show the sphere moving at once, without having to ask the server first, and correct itself
when the server turns out to disagree.

## Decision
**The movement rule is written once on each side.** The server has its rule in C++ (in the example, the code that moves a
player). The client gets a function with the same effect, written by the game: it takes the predicted properties, the
bytes of one input and the length of a tick, and changes the properties to what they become after that step. There is no
language for rules and no description of movement as data in the schema: that would be a second way of writing movement
that every game would have to fit into. The cost is that the game keeps two copies equal. Where they differ, the
difference is corrected and not hidden.

**Only the player's own entity is predicted,** and only the properties the game names. The core reads them as numbers
(a position as three, an angle, an integer and an enum as one each; a bit mask cannot be predicted). The client applies the
rule once for each `SendInput`, with a step of one over the tick rate that the server announced in its hello.

**The server says which input a state reflects, in the header of the `StateBuffer`.** The header gets a field,
`last_input_sequence`: the newest input of that connection that the state in the buffer already reflects, 0 if none yet. A
field in the header travels with the state that it confirms and costs no message of its own. The alternative, a new
message, would arrive on its own schedule and could say something about a state the client has not got. The change is
the one this record could not make while input had no confirmation: **the protocol version goes from 2 to 3**, and a
hello of version 2 is refused with `VersionMismatch`. The project is in alpha, so servers and clients of the earlier
version stop connecting to the new ones and have to be built from the same version. No other message changed.

**Where the server reads the sequence.** In a tick the game simulates first and the network phase runs after it, and the
network phase reads the inputs before it writes the buffers. So the state in a buffer was made before the inputs read
in the same phase, and it reflects the inputs read up to the phase before. The server therefore copies, at the end of
each network phase, the newest input delivered for each connection, and the buffer of the next tick carries that number.
This holds for any game that simulates before it calls `ReplicationServer::Update`. The number never goes down, counts
only the inputs that were delivered (not the ones dropped by the limit of 0017), belongs to the connection and not to
anything shared, and follows the count around 2^32.

**A buffer with only the header, for a player who is not moving.** A player who stands still, or pushes against a wall, has
no state to send, and the server sends nothing to a player with nothing to say. With a prediction that would never be
confirmed: the client would send one input after another, none would be confirmed, and after a while the history would be
full. So when the number to confirm has moved and there is nothing else to send, the server sends a buffer that holds the
header and the end of the records, a few bytes (`ServerStats::input_confirmations_sent` counts them). It only goes to a
player that is sending inputs, and only once for each step forward of the number. It is not counted against the budget of
bytes. The format did not change; the client applies such a buffer like any other.

**Reconciliation.** When a state buffer is applied, and it is newer than the last one used, the client drops the inputs
the server has used, takes the state of the player's entity from the server (the state it already had, if the buffer holds
no record of it) and applies the rule to the inputs that are left, in order. A buffer that is older than one already used
is applied as history and ignored for the prediction, so the prediction never goes back in time. If the entity leaves
the view, the prediction is dropped and it starts again when the entity is back. A disconnection, a refusal or a
protocol error drops it too, and keeps the configuration.

**The error, the slide and the jump.** The error of a correction is the distance, over the position properties, between
what was on the screen and the corrected state; angles and counts are not distances and are not counted. The correction
is not applied at once: what is shown starts from where it was and slides in a straight line to the corrected state in
100 milliseconds. If the error is above 2 meters it is not worth sliding and the shown state jumps. Both numbers can be
changed by the game. Two predictions of one state differ at least by the quantization of the server's state, which is a
step of the schema (1 centimeter in the example), so that is the smallest error the tests accept for a rule that matches.
The time of the slide is a number the caller passes in (`AdvancePrediction`); the core reads no clock.

**The history is 64 inputs.** That is a little over two seconds at 30 inputs a second, and about one at 60. The
engine's own horizons are shorter (a baseline of 32 ticks, resends within 30). When a new input has no room, because the
server has not confirmed any of the 64, the prediction pauses: what is shown goes back to the server's state, the history
is emptied and the overflow is counted, and the prediction starts again by itself with the next state that arrives,
instead of staying off for good after a network outage. A rule that fails, by returning false, throwing or leaving a number
that is not finite, ends the prediction until it is set again, and the game is told once; it never stops the client.

**What the game has to do.** The prediction counts one step for each input, so the game must send one input for every tick
of the server, with zeros when nothing is pressed. A client that stops sending leaves the server holding the last axes (the
example for up to half a second) while the predicted sphere has nothing to apply, and the two drift apart until the
server stops too. Inputs that arrive in a different rhythm from the one they were sent, because of jitter or loss, are
applied by the server at its own pace (it applies one step for each tick, with the axes of the last input it has), so the
rule applied to the same inputs can end up in a slightly different place for a while.

**How much error to expect.** Measured against the example server on the simulated network, with a rule equal to the
server's and an input for every tick, over 300 ticks of full push:

| Network | Error of a correction, worst | At the 95th percentile |
|---|---|---|
| No delay or jitter, or a fixed delay | 0.0044 m | 0.0042 m |
| Delay from 0 to 2 ticks | 0.27 m (1.0 step) | 0.26 m (1.0 step) |
| 30% loss, 10% duplication, delay from 0 to 2 ticks | 1.07 m (4.0 steps) | 0.63 m (2.4 steps) |

A step is the distance of one tick at full speed, 0.27 m. On a steady network the error stays under the quantization
of the schema. On a very bad one (heavy loss, duplication and jitter together) the corrected error can reach about
0.6 m at the 95th percentile and 1.1 m at worst; the slide covers it, and it was never above the 2 m of the jump. It goes
back to the quantization step once the inputs have stopped for a second. The way to remove it is to let the game tell the
core which input it applied (for example, by taking one input for each tick from a short queue); that needs an addition
to the game interface and is left for later.

**Left out on purpose:** smoothing of the other players, compensation of lag on the server, going back in time on the server,
prediction of entities the player does not control, a rule described as data in the schema, prediction of creating and
destroying entities, reconnecting into an old session, and cheat protection beyond what 0017 does.

**What was checked, and what was not.** The message, the server's number, the predictor, the client and its use against the
example server on a simulated network that loses and delays messages are covered by tests of the core and of the
server. They were run on Windows (MSVC) on the machine where this was written, and on Linux in a container with GCC 13
and Clang 18. The Godot calls are covered by a test that runs a Godot without a window against the example server, on
Windows and on Linux in a container with a Godot built from source; the continuous integration does not run Godot, and has
not run this change yet. Nobody has looked at the demo with a window, so how the prediction feels is not judged.

## Consequences
**Easier:** a player's movement shows at once, with a rule the game writes in the language it already uses, without a
protocol of its own, and an error is corrected without a visible snap in the usual cases.

**Harder:** the game keeps two copies of the rule equal, sends an input on every tick, and sees a transient error when its
inputs arrive in an uneven rhythm. A new protocol version breaks older clients and servers. The server sends a few bytes
more to a player that moves nothing but sends input.
