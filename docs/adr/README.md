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
