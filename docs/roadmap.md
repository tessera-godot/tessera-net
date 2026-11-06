# Roadmap

The order matters: each step depends on the previous one. Dates are intentionally absent; quality comes
before speed.

| Step | Deliverable | Status |
|---|---|---|
| 0 | Development environment, project skeleton, baseline measurements and targets | Done |
| 1 | Data core: entities, handles, fixed tick, spatial grid, jobs | Done |
| 2 | Schema and code generation (C++ server side, GDScript client side) | Planned |
| 3 | Replication: relevancy, dormancy, priority, bandwidth budget, delta, quantization, transport | In progress |
| 4 | Input, prediction and reconciliation; movement and proxies | Planned |
| 5 | Standalone server, export from the editor, network profiler | Planned |

Replication is built in slices:

| Slice | What it covers | Status |
|---|---|---|
| Relevancy | Which entities each player sees: ranges per type, groups, short ids, incremental updates | Done ([benchmarks](benchmarks.md#5-relevancy)) |
| Replicated state | Which properties are sent, quantization, changes only | First version done, slower than its goal ([benchmarks](benchmarks.md#6-replicated-state)); more property kinds and a faster path to come |
| Priority and bandwidth budget | What goes first when there is too much to send | Planned |
| Transport | Sending packets, reliable and unreliable messages | Planned |
| Client side | Applying what arrives, end-to-end test with many simulated players | Planned |

Later: lag compensation, visual state-tree editor, large-world streaming tools.

Every step ships with automated tests, documentation, a working example and a recorded benchmark.
