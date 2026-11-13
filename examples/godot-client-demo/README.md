# Godot client demo

A small Godot project that connects to the example server, describes its entities with a `TesseraSchema`, and draws one
sphere for each entity it is told about. When the server has players, it also sends the keys you press, so that you move
your own sphere. Its script is [`main.gd`](main.gd), and the guide
[Using the client in Godot](../../docs/guides/using-the-client-in-godot.md) explains each part.

## Run it

You need the extension built, the example server built, and Godot 4.5 or newer. The commands below are for Windows; on
Linux use the `linux-*` presets (the extension has been loaded into Godot on Windows only).

1. Build the extension, the demo project and the example server (the server is built with the `-extension` preset too):

   ```powershell
   cmake --preset windows-msvc-extension-debug
   cmake --build --preset windows-msvc-extension-debug --target tessera_extension_stage_demo tessera_example_server
   ```

   The first configure fetches godot-cpp; see [the guide](../../docs/guides/using-the-client-in-godot.md#build-the-extension)
   to use a copy you already have.

2. Start the example server in a terminal with `--players` (it listens on port 7777, keeps 100 entities walking in a
   square of 200 by 200 units, and makes one more entity for each player that connects):

   ```powershell
   ./build/windows-msvc-extension-debug/server/Debug/tessera_example_server.exe --players
   ```

   Without `--players` the server does not describe the player entities, so it does not match the schema in `main.gd`:
   the demo is refused and the label in the corner says why.

3. Let Godot read the project once. Godot only loads an extension that an import has listed, so on a folder the
   build has just put together, run:

   ```powershell
   godot --headless --path build/windows-msvc-extension-debug/extension/stage/demo --import
   ```

   (Opening the folder in the Godot editor does the same.) With Godot 4.7.2 the very first import of a folder with a
   new extension ends in a crash (exit code 134 on Linux, 139 on Windows) after the import has finished, and the second
   one finishes cleanly. It is not caused by this extension: an extension that only registers one empty class does the
   same, and one that registers no class does not. If the first import crashes, run the same command again.

4. Run the demo. This opens a window:

   ```powershell
   godot --path build/windows-msvc-extension-debug/extension/stage/demo
   ```

   Or press Play in the editor. It connects to `127.0.0.1:7777`. To use another server, pass its address after `--`:
   `godot --path <folder> -- --host=192.168.1.20 --port=7777`.

The label in the corner shows the state of the connection and how many entities the client has. If the server's
description of the entities and the one in `main.gd` differ, the label says the connection was rejected and why.

## Controls

W, A, S and D move your sphere, the green one. The other players' spheres are orange. The demo sends the keys 30 times
a second; the server moves your entity at up to 8 meters per second and stops it half a second after the input stops.
There is no prediction yet: the sphere moves when the server's answer arrives, one round trip after you press a key.

## What has been checked

Run in a headless Godot (no window), the demo project loads, connects to the example server and receives its 100 entities
without a script error; in the same kind of run, a client moves its own entity with the same input bytes and sees the
position the server works out. Nobody has looked at it with a window yet, so how it looks on screen, and how the keys
feel, are not confirmed.
