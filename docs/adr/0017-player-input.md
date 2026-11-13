# 0017. Player input

- **Status:** Accepted
- **Date:** 2026-11-13

## Context
Until now a client only listened. The protocol had two messages from the client to the server, the hello and the
confirmation of what it had applied ([0010](0010-transport-and-protocol.md)), so nothing a player did could reach the
game. Moving a character needs a way for the player to say what it wants, and the server to hand that to the game.

Prediction and reconciliation come later, and they will want to know which input the server has used. So each input
gets a sequence number now, even though nothing sends it back yet.

## Decision
**One generic message, `Input`, from the client to the server.** The core does not know what the bytes mean: an input
is a block of up to 16 bytes that the game defines, and a message carries up to 4 inputs, the newest and the 3 before
it, each with its sequence number. A client numbers its inputs 1, 2, 3, ... (after 4,294,967,295 it starts again at 1,
because 0 means "nothing was sent"). The largest message the client can produce, 4 inputs of 16 bytes, is at most 80
bytes, far below a packet of the usual 1,200.

**Unreliable, with each input repeated, instead of reliable.** An input that arrives late because a reliable channel
resent it is worse than an input that is lost: the next one already replaces it. Sending each input in four messages (its
own and the next three) covers a loss of up to three messages in a row. It does not need the server to confirm anything,
and a confirmation would have meant adding a field to a message that already exists, which changes bytes
([0010](0010-transport-and-protocol.md)). The confirmation can come with reconciliation, which needs it. The price: a
burst of more than three lost messages loses the inputs that only those carried, and the last input before the client
stops sending is not repeated.

**The server delivers each input once, in order, per connection.** `InputWindow` remembers the newest sequence number
delivered and the 32 before it. A number that has already been delivered is dropped as a repeat, and one older than the
newest, and never delivered, is dropped as late. Sequence numbers are compared as serial numbers, so the count can wrap
around 2^32 without stalling anyone. The state lives in the connection's own record, not in anything shared, in line with
[0015](0015-preparing-for-server-meshing.md). A client that jumps its sequence number to almost 2^31 only blocks itself:
what it sends next is older than where it jumped, and nobody else's input depends on it.

**A limit per connection and per tick.** A client can flood the server, so `ServerConfig::max_inputs_per_tick` (8 by
default, 1 to 64) is the most new inputs handed to the game for one connection in one `Update`. The rest are dropped and
counted without moving the window, so the next message that repeats them delivers them, still in order, as long as they
are within the 3 inputs a message repeats; older ones are lost. Every message is still decoded, so the limit caps what
reaches the game, not the work of reading messages. 8 per tick is exactly 240 inputs per second at 30 ticks per second,
which leaves no margin for jitter: a burst after a delay can go over it. `ServerStats` counts the inputs delivered,
repeated, late and dropped for being over the limit; the last counts drop events, so an input dropped again when a
later message repeats it counts again.

**The game decides what is valid.** The server hands each new input to the game through
`IGameLogic::OnPlayerInput(player, sequence, bytes)`, called while the network phase reads the transport, in order and
never after `OnPlayerDisconnected` for that player. The bytes come from a client, so nothing says they are what the game
expects. The example server reads two bytes, the axes along x and z from -127 to 127, cuts what is outside that range,
limits the speed whatever the client sends, does not let a diagonal be faster than a straight line, ignores an input
shorter than two bytes, and stops a player that sends nothing for half a second. That is as far as the checking goes
here.

**The protocol version goes from 1 to 2.** A server that does not know `Input` would drop it quietly, and the player
would stand still with no hint why. With a new version, a hello from an older client is refused with `VersionMismatch`
and the client is told. The project is in alpha, so clients and servers of 0.2.0-alpha stop connecting to the new ones,
and they have to be built from the same version.

**The example server makes a player entity only if you ask for it.** Started with `--players`, it adds entity type 2
(a position and the id of the connection that owns it), one entity for each connected player, with an observer that
follows it. It is opt-in because the hash of the schema goes into the hello, and a new type changes the hash: tests and
clients written for the example as it was would be refused. A client of the example with `--players` has to describe
type 2 as well.

**Left out on purpose:** prediction, reconciliation, interpolation, cheat protection beyond cutting values and the limit
above, a player's identity, reconnecting into an old session, sending input on the reliable channel, and any way for the
server to say which input it used.

**What was checked, and what was not.** The message, the window, the server's handling and the client's repeats are
covered by tests of the core and of the server, including a simulation in which the network loses 30 percent of what
the client sends. The example's player and the Godot calls are covered by a test that runs a Godot without a window
against the example server. That test ran on Windows on the machine where this was written, and on Linux in a container
with a Godot built from source; the continuous integration does not run Godot. Nobody has looked at the demo with a
window.

## Consequences
**Easier:** a game receives its players' input without writing a protocol of its own, and the sequence numbers are there
for prediction and reconciliation to build on.

**Harder:** without prediction, a player's entity moves one round trip after a key press, and the demo says so. The
repeats do not cover a burst of more than three lost messages. The version change breaks older programs for good,
which is the honest thing to do in an alpha but is a cost for anyone who built against the earlier one.
