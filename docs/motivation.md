# Motivation

## The problem

Godot's high-level multiplayer API is simple and works well for small and medium games. For large
open-world games with a dedicated server it runs into limits:

- Every replicated object is a node, and every node has its own synchronizer. Costs grow with
  *entities x clients*.
- There is no built-in way to say "this player only needs to hear about things within 300 meters".
- There is no dormancy: an entity that is standing still still costs CPU and bandwidth to check.
- Replication runs on a single thread.
- The dedicated server is still a full Godot process with a scene tree.

## What we measured

A benchmark on a single machine ([source](../bench/godot-replication/); localhost, one server core, 8 bot clients, every client seeing
every entity, one position property per entity at 20 Hz):

| Entities | Server frame rate (target 60) | Server CPU (1 core = 100%) | Bandwidth per client |
|---|---|---|---|
| 500 | 60 | 10.5% | 189 KB/s |
| 2,000 | 59.4 | 36.6% | 751 KB/s |
| 5,000 | 26.8 | 89.2% | 1.7 MB/s |
| 10,000 | 7.4 | 97.7% | 1.9 MB/s |

That works out to roughly 19 bytes per position update and about 0.85 million entity-client updates per
second per core. This is an order-of-magnitude result, not a rigorous benchmark: there was no network
latency or packet loss, and only positions were replicated.

## What we are building

A replication system where cost depends on what *changes* and what each player *can see*, not on how many
entities exist. It runs in a lean standalone server, uses all CPU cores, and is configured from the Godot
editor.

## Who it is for

Developers who want to build survival, open-world and large-scale multiplayer games in Godot, including
those moving from other engines who are used to having these systems available.
