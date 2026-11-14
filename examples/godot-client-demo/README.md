# Godot client demo

A small Godot project that connects to the example server, describes its entities with a `TesseraSchema`, and draws one
sphere for each entity it is told about. When the server has players, it also sends the keys you press, so that you move
your own sphere, and shows the other spheres a little in the past, smoothed. Its script is [`main.gd`](main.gd), and the guide
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

W, A, S and D move your sphere, the green one. The other players' spheres are orange. The white ones are the 100
entities the server keeps walking on their own; they have no owner, so nobody controls them. The demo sends the keys 30
times a second; the server moves your entity at up to 8 meters per second and stops it half a second after the input
stops.

Your sphere is predicted. `main.gd` has the movement rule of the example server written once more, in GDScript
(`predict`), and the client applies it to your sphere as soon as you press a key, without waiting for the server. When
the server's state arrives, the client drops the inputs the server has already used, starts again from the server's
position and applies the rest. If the two disagree, the difference fades over about 100 milliseconds, or the sphere
jumps if it is more than 2 meters. The label shows whether the prediction is running and the size of the last
correction. The prediction needs one input for every tick of the server, zeros included, so the demo keeps sending them
while no key is pressed.

The other spheres, the orange ones and the white ones, are interpolated. The server sends an entity's state at most 30
times a second, and drawn as it arrives an entity moves in steps. `main.gd` calls `client.set_interpolation` once, with
a delay of 2 ticks, and reads each of these spheres with `get_interpolated_property`: the client shows the entity at a
moment 2 ticks before the newest state it has, and moves it along the line between the two states it received on either
side of that moment. The price is that these spheres are shown about 66 milliseconds in the past (2 ticks at 30 ticks a
second), and your own sphere, which is predicted, is in the present, so the two are a little out of step. There is no
extrapolation: if a state is late, the sphere waits where it is until the state arrives, it does not guess ahead. Two
states more than 2.5 meters apart, or more than 8 ticks apart, are not blended: the sphere stays where it was and moves
on at the tick of the new state, so a sphere that was still and starts to move does not slide through the time it
stood. To change the delay, edit `INTERPOLATION_DELAY_TICKS` in `main.gd` (0 to 30); the label shows it.

## What has been checked

Run in a headless Godot (no window), the demo project loads, connects to the example server and runs without a script
error, with the connect and disconnect lines in its output. In that kind of run no key is pressed, so the correction
shown in the label stays at zero, and the sphere has not been moved with the keys. A client in a headless Godot, with
the same rule, moves its predicted value before the server answers and ends where the server puts the entity, and a
client that watches another player has an interpolated value that stays between the values received, lags behind the
last one and ends on it when the player stops. Nobody has looked at the demo with a window yet, so how it looks on
screen, how smooth the interpolated spheres are, and how the keys feel, are not confirmed.
