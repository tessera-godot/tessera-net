# Godot client demo

A small Godot project that connects to the example server, describes its entities with the script `example_schema.gd` (made from the server's schema file by `tessera_schema`), and draws one
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

   Without `--players` the server does not describe the player entities, so it does not match the schema in `example_schema.gd`:
   the demo is refused and the label in the corner says why.

3. Let Godot read the project once. Godot only loads an extension that an import has listed, so on a folder the
   build has just put together, run:

   ```powershell
   godot --headless --path build/windows-msvc-extension-debug/extension/stage/demo --import
   ```

   (Opening the folder in the Godot editor does the same.) The import also registers the class `ExampleSchema`, the
   name `main.gd` uses for the schema script. With Godot 4.7.2 the very first import of a folder with a
   new extension ends in a crash (exit code 134 on Linux, 139 on Windows) after the import has finished, and the second
   one finishes cleanly. It is not caused by this extension: an extension that only registers one empty class does the
   same, and one that registers no class does not. If the first import crashes, run the same command again.

4. Run the demo. This opens a window:

   ```powershell
   godot --path build/windows-msvc-extension-debug/extension/stage/demo
   ```

   Or press Play in the editor. It connects to `127.0.0.1:7777`. To use another server, pass its address after `--`:
   `godot --path <folder> -- --host=192.168.1.20 --port=7777`. A port that is not a whole number from 1 to 65535 is
   reported as an error, once, and the demo does not try to connect.

The label in the corner shows the state of the connection and how many entities the client has. If the server's
description of the entities and the one in `main.gd` differ, the label says the connection was rejected and why.

## The schema script

`example_schema.gd` is generated, not written by hand: `tessera_schema` makes it from `server/example/example_schema.json`,
the file the example server is made from too. It is kept in this folder so that the project opens in the editor as it
is. A test of the build (`tessera_schema_demo_check`) makes it again and fails when what is kept differs, which is what
happens after a change in the schema file. To make it again, from the root of the repository:

```text
tessera_schema generate --gdscript examples/godot-client-demo/example_schema.gd --gdscript-class ExampleSchema server/example/example_schema.json
```

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

## When the connection drops

If the connection to the server drops, the demo tries again on its own. It waits half a second, then 1, 2 and 4
seconds, and 5 seconds from the fifth try on, with no limit on the number of tries. Each wait is shifted by up to a fifth
either way, so that many clients that lost the same server do not all knock at the same moment. While it waits, the
first line of the label counts down, for example `reconnecting in 3 s (attempt 2)`. The wait starts when the last try
failed. A try at a server that is not there takes a while to fail on its own (about 7 or 8 seconds in runs on one
machine), so with the server stopped the tries come roughly every 8 seconds plus the wait.

It comes back as a new player. There is a new sphere for it, the prediction is set again on that sphere, as it was the
first time, and the interpolation setting is kept. It does not get its old entity back. If the old connection went
silent without a goodbye, the server only lets go of it after the time limit of its network library, which was about 6
seconds when measured on one machine (the library allows 5 to 30 seconds), and until then the other players see two
spheres for this one.

When the server refuses the connection on purpose, because it describes the entities differently or has no room left,
the demo prints a warning with the reason and does not try again. A different description would be refused again. A
server with no room may have a free slot later, but the demo stops at any refusal, by design. When the demo
closes the connection itself, as it does when the project quits, it does not try to come back either.

To see it, start the server with `--players`, open the demo, stop the server with Ctrl+C, and start it again.

## What has been checked

The script `example_schema.gd` was loaded by a headless Godot 4.7.2 on Windows and on Linux, and the hash of the schema
it builds was the one written in it and the one the example server expects. Run in a headless Godot (no window), the demo project loads, connects to the example server and runs without a script
error, with the connect and disconnect lines in its output. In that kind of run no key is pressed, so the correction
shown in the label stays at zero, and the sphere has not been moved with the keys. A client in a headless Godot, with
the same rule, moves its predicted value before the server answers and ends where the server puts the entity, and a
client that watches another player has an interpolated value that stays between the values received, lags behind the
last one and ends on it when the player stops.

The reconnection was run in a headless Godot against example servers started and stopped for the purpose. The same
headless test runs the demo script as a node: it schedules a try after a drop it did not ask for, makes the try from the
frame loop and not from the signal, stops after a refusal and does not come back after the demo itself let go. With the
server stopped and started again on the same port, the demo connected again on its own and the new server saw a player
join. Against a server that refuses it, it tried once and printed one warning. With an address that does not resolve,
it kept trying with growing waits. With a server that never came back, it kept trying for as long as it ran, without a
script error. Nobody has looked at the demo with a window yet, so how it looks on screen, how smooth the interpolated
spheres are, how the keys feel, and how the countdown in the label reads, are not confirmed.
