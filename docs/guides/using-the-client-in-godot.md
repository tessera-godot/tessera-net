# Using the client in Godot

This page shows how to connect a Godot project to a Tessera Net server and read the entities it sends. It is a first
version: the classes are small on purpose, and their names and details can still change (see
[record 0016](../adr/0016-the-godot-extension-client-first.md)).

## What it is

The extension adds two classes to Godot:

- `TesseraSchema` describes the properties the server sends for each type of entity.
- `TesseraClient` connects to a server, keeps what it receives, and tells your script about it with signals.

The server stays a separate program. It is not a Godot node and it does not run inside the editor. You start it as a
program (the [example server](../../server/README.md) is a good first one), and your Godot project is one of its
clients.

## What has been tried

The extension has been loaded into Godot 4.7.2 on Windows, and a headless Godot talked to the example server there over
real sockets. It was also loaded on Linux, in a container, into a Godot 4.7.2 built from source, and the same headless
test passed there against the example server (with the Debug library built by GCC 13 and by Clang 18). Both checks were
done locally. The continuous-integration workflow builds the extension on both systems but does not start Godot, because
we have no Godot binary for Linux that we could pin and verify. Nothing else has been tried.

## Build the extension

You need CMake 3.21 or newer, a C++20 compiler and Python 3. The first configure fetches godot-cpp, the library Godot
extensions are built with, at one fixed commit (about 400 MB with its history). If you already have a copy of that
commit, point to it and nothing is fetched.

```powershell
# Windows (Visual Studio 2022). Use the -release preset for a release library.
cmake --preset windows-msvc-extension-debug
cmake --build --preset windows-msvc-extension-debug --target tessera_extension_stage_demo
```

```bash
# Linux (Ninja and GCC or Clang)
cmake --preset linux-extension-debug
cmake --build --preset linux-extension-debug --target tessera_extension_stage_demo
```

To use a copy of godot-cpp you already have, add `-DFETCHCONTENT_SOURCE_DIR_GODOT_CPP=<path to the copy>` to the first
command. The copy has to be at the commit named in `extension/CMakeLists.txt`.

The last command puts the library, the `.gdextension` file and the demo project together in
`build/<preset>/extension/stage/demo/`. To use the extension in your own project, copy `bin/` and
`tessera_net.gdextension` from there into it.

The library is built for the `4.5` version of godot-cpp and says it needs Godot 4.5 or newer. It loads in Godot 4.7.2
(checked on Windows). The editor reads the `debug` entry of the `.gdextension` file, so use a debug library to try it in
the editor.

## Describe your entities

The client has to describe the entities the same way the server does: the same types, the same properties **in the same
order**, with the same kind and the same numbers for each one (step and range, bits, min, max, signed, count). If it
does not, the server refuses the connection and the client says so (see "When something goes wrong"). The names you
give the properties are only yours: they are not part of what is compared, so two descriptions that differ only in
names connect, and a typo in a name shows up as `null` when you read the value.

```gdscript
var schema := TesseraSchema.new()
schema.add_type(1, [{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0}])
```

`add_type(type_id, properties)` takes a type number from 0 to 255 and a list of properties. Each property is a
dictionary with a `name`, a `kind` and the keys that kind uses:

| `kind` | Keys | You get | Notes |
|---|---|---|---|
| `"position"` | `step`, `range` | `Vector3` | Rounded to steps of `step`; each axis from `-range` to `range`. |
| `"angle"` | `bits` (1 to 16) | `float`, in radians | Whole steps around the circle: 2^`bits` of them. |
| `"integer"` | `min`, `max`, `signed` | `int` | A whole number from `min` to `max`. `signed` is not worked out from `min`: it is `false` unless you set it, and a `min` below 0 with `signed` false is an error. It is part of what the server compares, so give the same value the server uses. |
| `"flag"` | none | `bool` | One on/off value. |
| `"mask"` | `bits` (1 to 64) | `int` | Up to 64 on/off values in one number. With 64 bits the top bit makes the number negative: the bits are the same. |
| `"choice"` | `count` (2 or more) | `int` | A number from 0 to `count - 1`. |

`add_type` returns `OK`, or `ERR_INVALID_PARAMETER` when something is wrong. In that case nothing is added, the reason
is in `get_error()` and in the editor's output, and the message names the type and the property, for example
`type 1, property "hp": unknown kind "float"`. These are errors too: a key that is not in the table, a key that belongs
to another kind (a `bits` on a `flag`), a key with the wrong type, and a missing key that has no default (`min` and
`max` for an integer, `step` and `range` for a position, `bits` for an angle or a mask, `count` for a choice; only
`signed` is optional). `is_valid()` tells whether the schema can be used (it needs at least one type), and `get_hash()`
is the number the server compares.

A `TesseraSchema` is a `Resource`: what you described is kept in its `types` property, so it can be saved as a `.tres`
file, loaded again and copied with `duplicate()`, and the copy has the same hash.

## Connect and read

```gdscript
extends Node3D

const DEFAULT_HOST := "127.0.0.1"
const DEFAULT_PORT := 7777
# How many times a second the input is sent, and how far the keys push (the server reads -127 to 127).
const INPUT_RATE := 30.0
const AXIS_MAX := 127
# The movement rule of the server, which the client needs to move the sphere before the answer comes.
const PLAYER_SPEED := 8.0
const WORLD_SIDE := 200.0
# The server runs at 30 ticks a second. The other spheres are shown this many ticks behind the newest state, so
# that there are two states to move between: 2 ticks is about 66 ms.
const TICK_RATE := 30.0
const INTERPOLATION_DELAY_TICKS := 2
const INTERPOLATION_SNAP_DISTANCE := 2.5
const OWN_COLOR := Color(0.2, 0.85, 0.3)
const OTHER_PLAYER_COLOR := Color(0.95, 0.6, 0.15)

var client := TesseraClient.new()
var spheres := {}  # entity id -> MeshInstance3D
var status := Label.new()
var input_clock := 0.0
var own_entity := -1  # the entity of this player, once the server has told us about it


func _ready() -> void:
	# The schema has to describe the entities exactly as the server does: the same types, the same properties in
	# the same order, and the same kind and numbers for each one.
	var schema := TesseraSchema.new()
	schema.add_type(1, [{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0}])
	# The players of a server started with --players: a position and the connection that owns the entity.
	schema.add_type(2, [
		{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0},
		{"name": "owner", "kind": "integer", "min": 0, "max": 65535, "signed": false},
	])
	client.schema = schema

	# Before connecting is fine: the setting stays through a reconnection.
	if client.set_interpolation(INTERPOLATION_DELAY_TICKS, INTERPOLATION_SNAP_DISTANCE) != OK:
		push_warning("No interpolation: " + client.get_last_error())

	client.entity_appeared.connect(_on_entity_appeared)
	client.entity_removed.connect(_on_entity_removed)

	var host := DEFAULT_HOST
	var port := DEFAULT_PORT
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--host="):
			host = argument.substr(7)
		elif argument.begins_with("--port="):
			port = int(argument.substr(7))

	if client.connect_to(host, port) != OK:
		push_error("Could not connect: " + client.get_last_error())

	_build_scene()


func _process(delta: float) -> void:
	# Nothing is read from the network, and no signal is sent, until poll() is called.
	client.poll()
	_send_input(delta)
	for id in spheres:
		# Your own sphere shows the prediction; the others show the interpolated value, a little in the past.
		var position = client.get_predicted_property(id, "position") if id == own_entity else client.get_interpolated_property(id, "position")
		if position != null:
			spheres[id].position = position
	var stats := client.get_prediction_stats()
	status.text = "%s, %d entities %s\nprediction: %s, error %.2f m\ninterpolation: %d ticks (%d ms)\nWASD moves the green sphere at once; the others are shown %d ms in the past, smoothed" % [
		_state_name(), spheres.size(), client.get_rejection(),
		"on" if client.is_predicting() else "off", stats["last_error"],
		INTERPOLATION_DELAY_TICKS, _delay_ms(), _delay_ms()]
```

This is the start of the [demo script](../../examples/godot-client-demo/main.gd), word for word. The rest of the file sends
the input (see [Send input](#send-input)), makes one sphere for each entity that appears, removes it when the entity goes,
builds the camera and the light, and shows the state on screen. The second type is the one the example server makes for
its players when it is started with `--players`; with a server that does not, the schema is refused, because it
describes more than the server does.

Call `poll()` once per frame. That is when the client reads the network, updates what it knows, and sends signals.
The schema is copied when you call `connect_to`: changing the resource afterwards does not change a client that is
already connecting.

| Call | What it does |
|---|---|
| `connect_to(host, port)` | Starts connecting. Returns `OK`, or `ERR_INVALID_PARAMETER` with the reason in `get_last_error()`: no valid schema, a port outside 1 to 65535, a host name that cannot be resolved, or a connection that could not be opened. A server that refuses you, or is not there, shows up later, as the state. |
| `disconnect_from_server()` | Leaves the server. Sends no signal. Harmless if you call it twice or before connecting. |
| `poll()` | Reads what arrived and sends the signals for it. |
| `send_input(bytes)` | Sends one input of the player and returns its sequence number, or 0 if nothing was sent. See [Send input](#send-input). |
| `get_connection_id()` | The id the server gave this client, or -1 while the state is not `CONNECTED`. |
| `set_prediction(...)`, `clear_prediction()`, `get_predicted_property(...)`, `is_predicting()`, `get_prediction_stats()` | Predict the player's own entity. See [Predict your player](#predict-your-player). |
| `set_interpolation(...)`, `clear_interpolation()`, `get_interpolated_property(...)`, `is_interpolating()`, `get_interpolation_stats()` | Show the other entities a little in the past, smoothed. See [Smooth the other entities](#smooth-the-other-entities). |
| `get_state()` | One of `TesseraClient.State.CONNECTING`, `HANDSHAKING`, `CONNECTED`, `REJECTED`, `DISCONNECTED`. A new client is `DISCONNECTED`. |
| `get_rejection()` | Why the server said no. Empty unless the state is `REJECTED`. |
| `get_last_error()` | The reason the last `connect_to` failed, or the last `poll()` hit a problem it could not report any other way. |
| `get_entities()` | A `PackedInt64Array` with the id of every entity the client has a state for. |
| `get_property(id, name)` | The value, already converted (see the table above), or `null` if the client does not have it. Never an error. |
| `get_snapshot()` | A dictionary of everything: `{id: {name: value}}`. |

### Signals

| Signal | When |
|---|---|
| `connected()` | The server accepted the client. |
| `rejected(reason: String)` | The server refused the client. |
| `disconnected()` | The connection ended, or never opened. Not sent when your own script called `disconnect_from_server()`. |
| `entity_appeared(id: int)` | The client now has the state of an entity. This is when the state arrived, not when the server made the entity visible. |
| `entity_removed(id: int)` | The entity left. When a connection drops, you get this for every entity first, and then `disconnected`. |

Inside a signal you may call the client again. `poll()` there does nothing. If a signal's handler calls
`disconnect_from_server()` or `connect_to()`, the signals that were still waiting from that read are dropped, because
they were about the old connection.

## Send input

`send_input(bytes)` sends one input of the player, as a `PackedByteArray` that the game on the server defines. The
[example server](../../server/README.md), started with `--players`, makes one entity for each connected player (type 2,
with a `position` and an `owner`, the id of the connection that owns it) and reads two bytes from each input: the axis
along x and the axis along z, each from -127 to 127 (a byte that GDScript sends as 0 to 255 is read as a number with a
sign, so 129 is -127). The demo sends them 30 times a second:

```gdscript
# Sends the keys as two bytes, the axis along x and the axis along z, 30 times a second while connected, zeros
# included: the prediction counts one step for each input, so the client sends one for every tick of the server.
# The server reads each as a number from -127 to 127 and moves the player's entity.
func _send_input(delta: float) -> void:
	if client.get_state() != TesseraClient.State.CONNECTED:
		return
	input_clock += delta
	if input_clock < 1.0 / INPUT_RATE:
		return
	input_clock = fmod(input_clock, 1.0 / INPUT_RATE)
	var x := 0
	var z := 0
	if Input.is_physical_key_pressed(KEY_D):
		x += AXIS_MAX
	if Input.is_physical_key_pressed(KEY_A):
		x -= AXIS_MAX
	if Input.is_physical_key_pressed(KEY_S):
		z += AXIS_MAX  # the camera looks toward -z, so S moves toward it
	if Input.is_physical_key_pressed(KEY_W):
		z -= AXIS_MAX
	client.send_input(PackedByteArray([x & 0xFF, z & 0xFF]))
```

- **What it returns.** The sequence number of the input, 1 for the first and then 2, 3, ..., or 0 when nothing was sent:
  the client is not `CONNECTED` (not yet, after `disconnect_from_server()`, after a refusal or after the server went
  away), or the array has more than 16 bytes. An empty array is a valid input. It is safe in every state and from inside
  a signal, and it never raises an error.
- **How it travels.** Each input goes in four messages: its own and the three after it. A lost message is usually
  covered by the next one, and nothing tells the client whether the server got an input.
- **Which entity is yours.** `get_connection_id()` is the id the server gave the client, and the example server puts it in
  the `owner` of the player's entity. The demo compares the two to color its own sphere.
- **Without a prediction** the sphere moves one round trip after the key press, because the server moves the entity and
  tells the client. See [Predict your player](#predict-your-player). The server moves a player at up to 8 meters per
  second whatever the client sends, and stops it half a second after the input stops.

## Predict your player

To move your own sphere at once, give the client the movement rule of your game, as a function, and the entity to
apply it to. The rule is the same one the server runs, written once more for the client (here, the rule of the example
server). The demo's rule agrees with the server's for the inputs of 2 bytes that the demo sends:

```gdscript
# The movement rule of the example server, for one input and one step of dt seconds: the two bytes are the axes
# along x and z (-127 to 127), the speed is at most PLAYER_SPEED and the player stays inside the world. This has
# to stay the same as the rule on the server; where the two differ, the server wins and the sphere is corrected.
# The two agree for the 2-byte inputs this script sends. A shorter input is ignored by the server, which keeps
# the last axes, while this rule does not move at all; the demo never sends one.
func predict(state: Dictionary, input: PackedByteArray, dt: float) -> Dictionary:
	if input.size() < 2:
		return state
	var position: Vector3 = state["position"]
	var dx := maxi(input.decode_s8(0), -127) / 127.0
	var dz := maxi(input.decode_s8(1), -127) / 127.0
	var length := sqrt(dx * dx + dz * dz)
	if length == 0.0:
		return state
	if length > 1.0:
		dx /= length
		dz /= length
	position.x = clampf(position.x + dx * PLAYER_SPEED * dt, 0.0, WORLD_SIDE)
	position.z = clampf(position.z + dz * PLAYER_SPEED * dt, 0.0, WORLD_SIDE)
	return {"position": position}
```

It is given to the client when your own entity appears, and the sphere reads the predicted value:

```gdscript
client.set_prediction(id, PackedStringArray(["position"]), predict)
...
var position = client.get_predicted_property(id, "position")
```

| Call | What it does |
|---|---|
| `set_prediction(entity, properties, rule)` | Starts predicting `entity` (the player's own) with `rule`, a `Callable(state: Dictionary, input: PackedByteArray, dt: float) -> Dictionary`. `properties` are the names of what the rule moves. Returns `OK`, or `ERR_INVALID_PARAMETER` (`ERR_BUSY` from inside the rule) with the reason in `get_last_error()`. The prediction starts when the next state arrives from the server. Safe in any state, but it needs a connection. |
| `clear_prediction()` | Stops predicting. A disconnection does the same. |
| `get_predicted_property(id, name)` | The value to show: the predicted and smoothed one for the predicted entity and its predicted properties while the prediction runs, and the same as `get_property` for anything else. |
| `is_predicting()` | Whether the prediction runs. |
| `get_prediction_stats()` | A dictionary: `state` (`"off"`, `"waiting"`, `"active"`, `"paused"` or `"failed"`), `reconciliations`, `snaps`, `rule_failures`, `history_overflows` and `last_error`, the size in world units of the last correction. |
| `smoothing_seconds`, `snap_distance` | How long a correction slides (0 to 5 seconds, 0.1 by default) and the distance above which the shown value jumps (above 0, 2 by default). |

- **The rule** gets the predicted properties by name in `state` (a position is a `Vector3`, an angle a float, an integer an
  int) and returns a `Dictionary` with the values after one step. A name you leave out keeps its value. Returning
  anything else than a `Dictionary`, or a value of the wrong type, turns the prediction off with one warning, and
  `get_predicted_property` gives what the server says. The game goes on.
- **The rule must not call back into the client.** It runs inside `send_input` and `poll`, so while it runs
  `send_input`, `poll`, `connect_to`, `disconnect_from_server`, `set_prediction`, `clear_prediction` and the two settings
  do nothing: `send_input` returns 0, the others return `ERR_BUSY` or nothing, and `get_last_error()` says why. Reading
  values is fine.
- **Send one input for every tick of the server, zeros included.** The client takes one step of the rule for each
  `send_input`, and the server takes one for each tick. The demo sends at 30 a second, even when no key is pressed. A
  client that stops sending leaves the server holding your last keys for up to half a second (in the example), and the
  two spheres drift apart until it stops.
- **The prediction starts after a state arrives** that the server sends because it has an input to confirm, so send inputs
  as soon as you call `set_prediction`. Before that, `get_predicted_property` is the server's value.
- **How much error to expect.** With the same rule on both sides and one input for every tick, the correction is within
  the quantization of the schema (1 centimeter in the example). In a very bad network (heavy loss, duplication and jitter
  together) the corrected error can reach about 0.6 m at the 95th percentile and 1.1 m at worst, covered by the slide. The
  way to remove it is for the game to tell the server's core which input it applied, which is not built yet.
- **The other spheres** are smoothed with the interpolation of the next section.

## Smooth the other entities

The server sends the state of an entity at most 30 times a second, so drawn as it arrives, any entity that is not yours
moves in steps. Interpolation shows each of them a few ticks in the past, and moves it along the line between the two
states the client received on either side of that moment. The demo turns it on once, before it connects, and reads the
spheres of the others with `get_interpolated_property`:

```gdscript
if client.set_interpolation(INTERPOLATION_DELAY_TICKS, INTERPOLATION_SNAP_DISTANCE) != OK:
	push_warning("No interpolation: " + client.get_last_error())
...
var position = client.get_predicted_property(id, "position") if id == own_entity else client.get_interpolated_property(id, "position")
```

| Call | What it does |
|---|---|
| `set_interpolation(delay_ticks = 2, snap_distance = 2.5)` | Turns interpolation on. `delay_ticks` (0 to 30) is how far behind the newest state the entities are shown; the server runs at 30 ticks a second in the example, so 2 is about 66 ms. `snap_distance` (above 0, in world units) is how far apart two states may be and still be blended. Returns `OK`, or `ERR_INVALID_PARAMETER` with the reason in `get_last_error()`. Works before and after connecting, and stays through a reconnection. |
| `clear_interpolation()` | Turns it off and forgets the settings. |
| `get_interpolated_property(id, name)` | The value to show, with no calls to the game. For the entity you predict, the same as `get_predicted_property` (the last state of the server when the prediction is not running). Otherwise, with interpolation running: a position or an angle blended between two states, and anything else (an integer, a flag, a mask, a choice) the value in force at the time shown. In every other case, the same as `get_property`. |
| `is_interpolating()` | True once interpolation is set and a state has arrived since the connection. |
| `get_interpolation_stats()` | A dictionary: `state` (`"off"`, `"waiting"` or `"running"`), `delay_ticks`, `snap_distance`, `states` kept, `states_ignored`, `gap_holds`, `snaps`, `stalls` and `resyncs`, and `lag_ticks`, how many ticks behind the newest one the time shown is. |

- **The time moves with `poll()`,** by the time that passed since the last call (at most a quarter of a second, so a frame
  that took long does not make it jump). Call `poll()` every frame.
- **The price is the delay.** The other entities are shown about 66 ms in the past with the default of 2 ticks, and your
  own sphere, which is predicted, is in the present, so what you chase or touch is a little behind where it really is.
  A larger delay gives more room to ride out a late state and costs more lag. The clock aims at the delay behind the
  newest state and comes back to it after a stall, within a band of a quarter of the delay (at least a quarter of a
  tick, at most 2 ticks).
- **There is no extrapolation.** If a state arrives later than the delay, the entity waits where it is until it comes; it
  does not guess where it is going.
- **A teleport, or a long silence, is not slid through.** Two neighbouring states more than `snap_distance` apart, or
  more than 8 ticks apart, are not blended: the entity stays at the older and goes to the newer at the tick of the newer.
  An entity that stood still, and then moved, does not walk the whole silence. Choose `snap_distance` above how far your
  fastest entity goes in 8 ticks: speed times 8 divided by the tick rate (a player at 8 m/s moves 2.13 m, under the 2.5
  default). The gap of 8 ticks is not a setting of the extension.
- **An angle goes the short way round,** and an integer, a choice and a set of flags change all at once, at the tick of the
  state that brings them, so they lag `get_property` by the delay too (a health value changes about 66 ms later at 30 Hz). A position is blended on the values the schema stands for, not on its steps of 1 centimeter.
- **`get_property` still gives the last value received,** and `get_interpolated_property` gives the same until you call
  `set_interpolation`.
- **The calls are safe inside a prediction rule:** unlike `send_input` and the others, they do not call back into the game,
  so they are not refused.
- **What it costs.** In a build with optimization, taking in a tick of states for 10,000 visible entities took under 0.3
  ms and reading every position about 0.12 ms on the machine where it was measured; each entity holds about 260 bytes
  while interpolation is on. It costs nothing while it is off.

## When something goes wrong

- **The schema does not match the server's.** The state becomes `REJECTED`, the `rejected` signal fires and
  `get_rejection()` says that the schema is not the same and tells you what to compare: the types, the order of the
  properties, the kind of each one and its numbers (bits, step and range, min, max, signed, count). The client never
  connects quietly with values it would read wrongly.
- **The server is not there.** The connection times out after a few seconds and you get `disconnected`.
- **The host name cannot be found.** `connect_to` returns `ERR_INVALID_PARAMETER` at once, `get_last_error()` says that
  the address could not be resolved, the state stays `DISCONNECTED` and no signal is sent, because nothing was ever
  connected. Looking a name up blocks the frame until the system answers or gives up, which for a name that does not
  exist can take a second or more; an IP address needs no lookup.
- **The server is full.** The state becomes `REJECTED` and the reason says the server is full.
- **Using the client at the wrong moment.** Every call is safe in every state: before connecting and after
  disconnecting, `get_entities()` is empty, `get_property()` is `null`, `poll()` does nothing, `send_input()` returns 0
  and `get_connection_id()` returns -1.

## What the client does not do

The client sends input (see [Send input](#send-input)), can predict the player's own entity (see
[Predict your player](#predict-your-player)) and can show the other entities smoothed, a little in the past (see
[Smooth the other entities](#smooth-the-other-entities)). It does not extrapolate, does not compensate for the delay of
the other entities when you aim at them, has no code generation and no shared schema file with the server (you write the
description twice), and the server is not a Godot node. There is no editor plugin yet.

## Limits to know about

- **Entity ids are `int`s with a sign.** The id has 64 bits, and a server with shard 2048 or higher sets the top bit, so
  its ids are negative in GDScript. They are still unique, and `int(id)` is what you pass back to `get_property`.
- **A 64-bit `mask` is a signed `int`**, as described in the table.
- **`entity_appeared` means the state arrived.** An entity the server can see but has not sent yet is not listed.
- **`poll()` cost.** It looks at every entity the client holds, so its cost grows with their number. No measurement of
  it is published.
