# Roadmap

The order matters: each step depends on the previous one. Dates are intentionally absent; quality comes
before speed.

| Step | Deliverable | Status |
|---|---|---|
| 0 | Development environment, project skeleton, baseline measurements and targets | Done |
| 1 | Data core: entities, handles, fixed tick, spatial grid, jobs | Done |
| 2 | Schema and code generation (C++ server side, GDScript client side) | Planned |
| 3 | Replication: relevancy, dormancy, priority, bandwidth budget, delta, quantization, transport | In progress |
| 4 | Input, prediction and reconciliation; movement and proxies | In progress: the player's input reaches the server ([record 0017](adr/0017-player-input.md)) and the player's own entity is predicted and reconciled ([record 0018](adr/0018-prediction-and-reconciliation.md), proposed); smoothing of the other players is still to come |
| 5 | Standalone server, a first Godot client, export from the editor, network profiler | In progress |

Replication is built in slices:

| Slice | What it covers | Status |
|---|---|---|
| Relevancy | Which entities each player sees: ranges per type, groups, short ids, incremental updates | Done ([benchmarks](benchmarks.md#5-relevancy)) |
| Replicated state | Which properties are sent, quantization, changes only | First version done, slower than its goal ([benchmarks](benchmarks.md#6-replicated-state)); more property kinds and a faster path to come |
| Priority and bandwidth budget | What goes first when there is too much to send, and how often to repeat what is not confirmed | First version done: bytes within the goal, priority by distance to come ([benchmarks](benchmarks.md#7-the-whole-network-path)) |
| Transport and client side | A transport interface, a simulated network, the protocol, the server and the client, end-to-end tests with many simulated players | First version done, bytes within the goal; the time is close to its goal at 1,000 moving entities per tick and far from it at 33,333 ([benchmarks](benchmarks.md#7-the-whole-network-path)); an optional transport over real sockets (ENet, [record 0013](adr/0013-enet-as-the-first-real-transport.md)) exists, and a head-to-head run against Godot's built-in replication is measured ([benchmarks](benchmarks.md#8-tessera-net-and-godots-built-in-replication-same-scenario)); a server library with a small example program ([record 0014](adr/0014-the-server-as-a-library-plus-an-example.md)) and a CI workflow for Ubuntu and Windows are added; a first Godot client (the extension `tessera_net` and a demo project, [record 0016](adr/0016-the-godot-extension-client-first.md)) has been built and loaded in Godot 4.7.2 on Windows, and on Linux it is compiled and tested without Godot; the client can send the player's input to the server ([record 0017](adr/0017-player-input.md)) and predict the player's own entity ([record 0018](adr/0018-prediction-and-reconciliation.md), proposed); the editor tools and the server as a Godot node are still to come |

The time of the network phase is a curve, not a point: it grows with the entities that move in a tick, and the goal for it
is now written that way ([record 0012](adr/0012-network-phase-as-a-curve.md); the measured curve is in the
[benchmarks](benchmarks.md#the-time-of-the-phase-by-entities-moving)):

| Entities moving per tick | One thread | Eight workers |
|---|---|---|
| 1,000 (reference load) | 3.3 ms | 1.0 ms |
| 2,985 | 7.0 ms | 2.5 ms |
| 10,000 | 17.0 ms | 5.0 ms |
| 33,333 (stress point) | 38.5 ms | 11.0 ms |
| 33,333, long term | 3.3 ms | 1.0 ms |

The 1,000 row is a reference load that has not been checked against real games. The next steps follow the curve: the
cost between 1,000 and 10,000 moving entities, where the records written per player still grow, is where to look first.

Preparation for server meshing is limited to a set of rules for the API, written down in
[0015](adr/0015-preparing-for-server-meshing.md). Server meshing itself is not planned.

Later: lag compensation, visual state-tree editor, large-world streaming tools.

Every step ships with automated tests, documentation, a working example and a recorded benchmark.
