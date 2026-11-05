# Concepts explained simply

You do not need to know these terms to start. Come back here when you meet one.

**Dedicated server.** A program that runs the game world for everyone, without a screen. Players connect
to it. It is the single source of truth.

**Replication.** Copying the state of the game world from the server to the players, so everyone sees the
same thing.

**Entity.** Anything in the world that the server keeps track of: a player, a zombie, a tree, a dropped
item.

**Relevancy (interest management).** Deciding which entities a given player needs to know about.
A player in the north of the map does not need updates about a tree in the south.

**Area of interest (AOI).** The region around a player in which entities are relevant. Usually a circle or
a set of grid cells.

**Dormancy.** An entity that is not changing goes to sleep and costs nothing until something changes it.

**Priority and bandwidth budget.** Each player can only receive so much data per second. When there is too
much to send, the most important things go first (for example, nearby players before faraway ones).

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
