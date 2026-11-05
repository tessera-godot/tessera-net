# 0004. Entities are stored in per-type paged tables

- **Status:** Accepted
- **Date:** 2026-11-05

## Context
The replication layer must find what changed, serialize it and send it for up to a million entities
every tick. That work is dominated by how the data sits in memory. A general entity-component-system
library lets you add and remove components at any time, but it pays for that flexibility on every
operation.

## Decision
Store the entities of each type in a table laid out as a *struct of arrays*, in pages of 4096 slots.
Pages are allocated on demand and never move. A slot keeps its index for the life of the entity, has a
generation counter, and has an alive bit and a dirty bit in per-page bitsets. Destroyed slots go to a
free list; a slot whose generation counter runs out is retired.

We measured this layout against a general-purpose ECS library (EnTT 3.16) in a throwaway benchmark with
1,000,000 entities on one thread:

| Operation | Per-type table | General ECS |
|---|---|---|
| Create 1 M | 16 ms | 94 ms |
| Destroy 1 M | 6 ms | 196 ms |
| Churn (destroy and recreate 10%, ten times) | 208 ms | 1,720 ms |
| Iterate 1 M | 1.5 ms | 3.2 ms |
| Serialize the 10% that changed | 0.9 ms | 4.9 ms (19 ms with tag components) |

The comparison favours the table because it does less: it cannot add components to a live entity or
answer arbitrary queries. The numbers show what that generality costs for this use, not that the other
library is slow. Memory and the library's grouping features were not measured.

## Consequences
- Fast creation, destruction, iteration and dirty scans, with predictable memory.
- An entity cannot gain or lose properties while alive. Rare states are handled by a state field in the
  same table, by optional sparse side tables, or by destroying and recreating the entity.
- The layout is known at compile time, which suits generating code from a schema.
