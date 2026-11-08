# Concepts explained simply

You do not need to know these terms to start. Come back here when you meet one.

**Dedicated server.** A program that runs the game world for everyone, without a screen. Players connect
to it. It is the single source of truth.

**Replication.** Copying the state of the game world from the server to the players, so everyone sees the
same thing.

**Quantization.** Storing a number with less precision than a `float` has, as a whole number of small
steps: a position to the centimetre, an angle in 1,024 steps. It makes the numbers small and easy to compare.

**Delta.** Sending only what differs from a state the other side already has, instead of everything.

**Confirmation (ack).** The player telling the server that a buffer arrived. The server writes the next ones
against the last state the player confirmed, so a lost packet never leaves the player out of step.

**Transport.** What carries messages between the server and a player. The rest of the code only knows its
interface, so the network library underneath can be replaced.

**Reliable and unreliable messages.** A reliable message always arrives, in the order it was sent, or the
connection fails. An unreliable one may be lost, repeated or arrive out of order, and is cheap. Visibility
events go the first way, state the second.

**Entity.** Anything in the world that the server keeps track of: a player, a zombie, a tree, a dropped
item.

**Relevancy (interest management).** Deciding which entities a given player needs to know about.
A player in the north of the map does not need updates about a tree in the south.
To stay cheap, the server does not look around every player on every tick: it remembers where it last looked
(the *anchor*) and looks again only when the player has moved a few metres.

**Area of interest (AOI).** The region around a player in which entities are relevant. Usually a circle or
a set of grid cells.

**Observer.** A point a player looks from, such as their camera. The server uses observers to decide what
is nearby. A player can have more than one, for example a camera and a drone.

**Hysteresis.** A small extra distance in which something that is already visible stays visible. Without
it, an entity standing exactly at the edge of the range would appear and vanish on every update.

**Inclusion and exclusion groups.** Sets of entities the game uses to make exceptions to the normal
rules: an exclusion group hides its members from the players it does not allow, an inclusion group shows
its members to the players it allows.

**Compact id.** A short number that stands for an entity on one player's connection. It costs far fewer
bytes than the entity's full 64-bit id.

**Dormancy.** An entity that is not changing goes to sleep and costs nothing until something changes it.

**Priority and bandwidth budget.** Each player can only receive so much data per second. When there is too
much to send, the most important things go first. Today that means what has waited longest, weighted by the type
of entity; nearby before faraway is not built yet.

**Network LOD.** Level of detail for networking: far-away things are updated less often than near ones.

**Delta compression.** Sending only what changed since the last update, not the whole state.

**Quantization.** Storing numbers with just enough precision. A position that needs centimeters does not
need to be sent as a 32-bit float.

**Tick.** One step of the server simulation. A 30 Hz server does 30 ticks per second.

**Prediction and reconciliation.** The player's game moves your character immediately (prediction) and
quietly corrects it if the server disagrees (reconciliation), so controls feel instant even with lag.

**Proxy.** A copy of an entity on a player's machine. The one you control is the *owner*; the ones you
only watch are *simulated proxies*.

**Determinism.** The same inputs always produce the same results, on any machine. It is what makes
prediction and replays reliable.

**Server meshing.** Splitting one big world across several server processes that cooperate. Not part of
the first release, but the design keeps the door open.
