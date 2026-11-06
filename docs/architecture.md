# Architecture

This is the planned architecture. Pieces are built one at a time; see the [roadmap](roadmap.md).

## Layers

```
 Godot editor / game (GDScript)         -> addons/tessera_net     (plugin, editor tools)
                 |
 GDExtension                            -> extension/            (binds the core to Godot)
                 |
 Core library (C++20, no Godot)         -> core/                 (the heart of the project)
                 |
 Standalone dedicated server            -> server/               (uses the core, no engine)
```

The same core is compiled twice: into the GDExtension used by the editor and clients, and into the
standalone server. This guarantees client and server agree on the rules.

## Core responsibilities

1. **Data**: entities stored in contiguous arrays (struct of arrays), identified by compact handles with a
   generation counter and a globally unique id.
2. **Simulation loop**: fixed tick, decoupled from rendering.
3. **Spatial grid**: the base for relevancy and, later, for splitting the world across servers.
4. **Replication**: relevancy, dormancy, priority, bandwidth budget, delta, quantization. Relevancy
   (deciding what each player sees) is implemented; see the [relevancy reference](reference/relevancy.md).
5. **Transport**: behind an interface, so the network library can be replaced.
6. **Prediction**: input commands, reconciliation, proxies.
7. **Jobs**: work split across CPU cores.

## The tick

The simulation runs in fixed steps. Each tick has five phases (input, simulate, structure, grid, network).
Creating and destroying entities is queued during the simulate phase and applied, in a deterministic
order, in the structure phase, so every other phase can run on many threads without locks. See the
[core reference](reference/core.md).

## One schema, two sides

Entities and their replicated properties are described once, in the editor. From that description the
project generates C++ for the server and GDScript bindings for the client, plus the serialization code, so
the two sides never drift apart.

## Designed for growth

Even though the first release targets a single server process, the design avoids choices that would block
a multi-server world later: globally unique entity ids, fully serializable entity state, authority stored
as data, and a grid that can be partitioned.
