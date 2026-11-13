# Architecture decision records

Short documents that capture important decisions and the reasoning behind them. Use
[`template.md`](template.md) for new ones. Records are never deleted; a decision that changes is marked
*Superseded* and linked to its replacement.

| Id | Decision | Status |
|---|---|---|
| [0001](0001-engine-independent-core.md) | The core library does not depend on Godot | Accepted |
| [0002](0002-standalone-dedicated-server.md) | The dedicated server is a separate program | Accepted |
| [0003](0003-library-policy.md) | Library policy | Accepted |
| [0004](0004-per-type-paged-tables.md) | Entities are stored in per-type paged tables | Accepted |
| [0005](0005-entity-identification.md) | Entity identification | Accepted |
| [0006](0006-relevancy-by-type-and-groups.md) | Relevancy by entity type, with hysteresis and groups | Accepted |
| [0007](0007-relevancy-first-version.md) | The first relevancy version is accepted at its measured cost | Superseded by 0008 |
| [0008](0008-incremental-relevancy.md) | Relevancy is updated incrementally, with a small tolerance | Accepted |
| [0009](0009-replicated-state.md) | Replicated state against the last confirmed state, accepted at its measured cost | Accepted |
| [0010](0010-transport-and-protocol.md) | Transport by messages, a protocol with numbered event batches, accepted at its measured cost | Accepted |
| [0011](0011-priority-and-bandwidth-budget.md) | Resends by measured round trip, priority by waiting time and weight, and a byte budget for each player | Accepted |
| [0012](0012-network-phase-as-a-curve.md) | The time of the network phase as a curve, goals by moving entities | Accepted |
| [0013](0013-enet-as-the-first-real-transport.md) | ENet as the first real transport | Accepted |
| [0014](0014-the-server-as-a-library-plus-an-example.md) | The server as a library plus an example | Accepted |
| [0015](0015-preparing-for-server-meshing.md) | Preparing for server meshing | Accepted |
| [0016](0016-the-godot-extension-client-first.md) | The Godot extension: client first | Accepted |
| [0017](0017-player-input.md) | Player input | Accepted |
