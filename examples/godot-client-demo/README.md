# Godot client demo

A small Godot project that connects to the example server, describes its entities with a `TesseraSchema`, and draws one
sphere for each entity it is told about. Its script is [`main.gd`](main.gd), and the guide
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

2. Start the example server in a terminal (it listens on port 7777 and keeps 100 entities walking in a square of 200 by
   200 units):

   ```powershell
   ./build/windows-msvc-extension-debug/server/Debug/tessera_example_server.exe
   ```

3. Let Godot read the project once. Godot only loads an extension that an import has listed, so on a folder the
   build has just put together, run:

   ```powershell
   godot --headless --path build/windows-msvc-extension-debug/extension/stage/demo --import
   ```

   (Opening the folder in the Godot editor does the same.)

4. Run the demo. This opens a window:

   ```powershell
   godot --path build/windows-msvc-extension-debug/extension/stage/demo
   ```

   Or press Play in the editor. It connects to `127.0.0.1:7777`. To use another server, pass its address after `--`:
   `godot --path <folder> -- --host=192.168.1.20 --port=7777`.

The label in the corner shows the state of the connection and how many entities the client has. If the server's
description of the entities and the one in `main.gd` differ, the label says the connection was rejected and why.

## What has been checked

Run in a headless Godot (no window), the demo project loads, connects to the example server and receives its 100 entities
without a script error. Nobody has looked at it with a window yet, so how it looks on screen is not confirmed.
