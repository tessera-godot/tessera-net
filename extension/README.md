# extension

The Godot GDExtension that lets a Godot project connect to a Tessera Net server as a client. It is a first version: two
classes, `TesseraSchema` and `TesseraClient`, built on top of the core and its ENet transport. The server is not part of
it; it stays a program of its own. The guide [Using the client in Godot](../docs/guides/using-the-client-in-godot.md) shows
how to use it, and [record 0016](../docs/adr/0016-the-godot-extension-client-first.md) explains the choices.

## What is here

| Path | What it holds |
|---|---|
| `logic/` | Everything that does not need Godot (`tessera_extension_logic`): building a schema from a description, converting values, and the client session. |
| `src/` | The Godot side (`tessera_net_godot`, a shared library): the two classes, the registration and the entry point. |
| `tessera_net.gdextension.in` | The `.gdextension` file, with the library names for Windows and Linux. |
| `tests/` | Tests of the logic (no Godot needed), the check of what the library exports, and the end-to-end test in a headless Godot. |

## The classes

| Class | What it is | Main members |
|---|---|---|
| `TesseraSchema` (`Resource`) | The types of entity and their properties, as the server has them. | `add_type`, `is_valid`, `get_error`, `get_hash` |
| `TesseraClient` (`RefCounted`) | A connection to a server. | `connect_to`, `disconnect_from_server`, `poll`, `send_input`, `get_connection_id`, `set_prediction`, `clear_prediction`, `get_predicted_property`, `is_predicting`, `get_prediction_stats`, `set_interpolation`, `clear_interpolation`, `get_interpolated_property`, `is_interpolating`, `get_interpolation_stats`, `get_state`, `get_rejection`, `get_last_error`, `get_entities`, `get_property`, `get_snapshot`, the properties `schema`, `smoothing_seconds` and `snap_distance`, and the signals `connected`, `disconnected`, `rejected`, `entity_appeared` and `entity_removed` |

The interface is the first version and can change.

## Build

It is built only with `-DTESSERA_BUILD_EXTENSION=ON`, which is off by default. The option also needs
`TESSERA_BUILD_ENET` and `TESSERA_BUILD_SERVER`, and the first configure fetches godot-cpp at one pinned commit. The
presets set all of that:

```powershell
cmake --preset windows-msvc-extension-debug      # or windows-msvc-extension-release
cmake --build --preset windows-msvc-extension-debug
ctest --preset windows-msvc-extension-debug
```

```bash
cmake --preset linux-extension-debug             # or linux-extension-release
cmake --build --preset linux-extension-debug
ctest --preset linux-extension-debug
```

To use a copy of godot-cpp you already have (at the commit named in `CMakeLists.txt`), add
`-DFETCHCONTENT_SOURCE_DIR_GODOT_CPP=<path>` to the first command. The library is written to
`build/<preset>/extension/lib/`. The target `tessera_extension_stage_demo` puts the library and the demo project together
in `build/<preset>/extension/stage/demo/`, ready to open in Godot.

## What is checked, and where

- The logic has its own tests, `tessera_extension_tests`, which run with the simulated network and need no Godot.
- `tessera_extension_exports` fails if the library exports anything except its entry point, so that its copy of ENet cannot
  clash with the one inside Godot.
- `tessera_extension_godot` loads the library in a headless Godot, starts the example server, and checks that a script
  connects, receives the entities and reads their values, and, against a second example server started with `--players`,
  that `send_input` moves the player's entity and two connections do not move each other's, that a prediction follows the server, and that a client watching another player reads an interpolated value that stays between the values received, lags the last one and ends on it when the player stops. It is registered only if you pass
  `-DTESSERA_EXTENSION_GODOT=<path to a Godot executable that runs without a window>`: no Godot is fetched by the build.
  It was run with Godot 4.7.2 on Windows, and on Linux in a container with a Godot 4.7.2 built from source (Debug
  library, built by GCC 13 and by Clang 18).
- Loading the library into Godot is checked locally, not by the CI workflow, which builds and tests the extension without
  starting Godot: we have no Godot binary for Linux that we could pin and verify.
- The first time Godot imports a project that has the extension, Godot 4.7.2 itself crashes at the end of the import
  (on Windows and on Linux). The test script repeats the import once, and the second one is clean.
