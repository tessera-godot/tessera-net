# 0002. The dedicated server is a separate program

- **Status:** Accepted
- **Date:** 2026-11-05

## Context
A headless Godot export still runs the full engine and scene tree. Large worlds need tight control of
memory, threads and tick timing.

## Decision
The production server is a standalone binary built from the core. A headless Godot process with the
extension loaded remains available as a development mode.

## Consequences
- Server-side game logic is written in C++ (or in data authored in the editor), not in GDScript.
- Clients may use GDScript for everything that is not authoritative.
- Two run modes must be kept compatible.
