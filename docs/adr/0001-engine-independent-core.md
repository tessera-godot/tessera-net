# 0001. The core library does not depend on Godot

- **Status:** Accepted
- **Date:** 2026-11-05

## Context
The same simulation and replication logic must run in the Godot editor and clients and in a lean dedicated
server. Godot's scene tree and API are not designed for millions of entities or for a server without the
engine.

## Decision
The `core/` library is plain C++20 with no Godot headers. A separate GDExtension (`extension/`) binds it to
Godot. The standalone server links the core directly.

## Consequences
- The server can run without the engine, with its own threading model.
- Client and server share exactly the same code.
- We maintain a binding layer, and Godot types must be converted at the boundary.
