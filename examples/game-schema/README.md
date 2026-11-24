# game-schema

A small server project whose entities are described once, in `schema.json`. The build makes the C++ header for the
server and `godot/game_schema.gd` for a Godot client from that file, and `main.cpp` uses the header.

Build it where it is:

    cmake -S examples/game-schema -B build/game-schema
    cmake --build build/game-schema

The guide [Describe your entities once](../../docs/guides/describe-your-entities-once.md) goes through it step by step.
