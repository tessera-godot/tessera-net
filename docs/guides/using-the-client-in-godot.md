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
real sockets. On Linux it compiles with GCC and Clang and passes its tests that need no Godot (checked in a container),
and its library exports only its entry point; it has not been loaded into a Godot on Linux. The continuous-integration
workflow builds it on both systems, and it has not run yet. Nothing else has been tried.

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
order**, and the same step and range for every position. If it does not, the server refuses the connection and the
client says so (see "When something goes wrong"). The names you give the properties are only yours: they are not part of
what is compared, so two descriptions that differ only in names connect, and a typo in a name shows up as `null` when
you read the value.

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
| `"integer"` | `min`, `max`, `signed` | `int` | A whole number from `min` to `max`. `signed` is `false` if `min` is not below 0. |
| `"flag"` | none | `bool` | One on/off value. |
| `"mask"` | `bits` (1 to 64) | `int` | Up to 64 on/off values in one number. With 64 bits the top bit makes the number negative: the bits are the same. |
| `"choice"` | `count` (2 or more) | `int` | A number from 0 to `count - 1`. |

`add_type` returns `OK`, or `ERR_INVALID_PARAMETER` when something is wrong. In that case nothing is added, the reason
is in `get_error()` and in the editor's output, and the message names the type and the property, for example
`type 1, property "hp": unknown kind "float"`. A key that is not in the table, or has the wrong type, is an error too.
`is_valid()` tells whether the schema can be used (it needs at least one type), and `get_hash()` is the number the
server compares.

## Connect and read

```gdscript
extends Node3D

const DEFAULT_HOST := "127.0.0.1"
const DEFAULT_PORT := 7777

var client := TesseraClient.new()
var spheres := {}  # entity id -> MeshInstance3D
var status := Label.new()


func _ready() -> void:
	# The schema has to describe the entities exactly as the server does: the same types, the same properties in
	# the same order, and the same step and range for each position.
	var schema := TesseraSchema.new()
	schema.add_type(1, [{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0}])
	client.schema = schema

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


func _process(_delta: float) -> void:
	# Nothing is read from the network, and no signal is sent, until poll() is called.
	client.poll()
	for id in spheres:
		var position = client.get_property(id, "position")
		if position != null:
			spheres[id].position = position
	status.text = "%s, %d entities %s" % [_state_name(), spheres.size(), client.get_rejection()]
```

This is the start of the [demo script](../../examples/godot-client-demo/main.gd), word for word. The rest of the file makes
one sphere for each entity that appears, removes it when the entity goes, builds the camera and the light, and shows the
state on screen.

Call `poll()` once per frame. That is when the client reads the network, updates what it knows, and sends signals.
The schema is copied when you call `connect_to`: changing the resource afterwards does not change a client that is
already connecting.

| Call | What it does |
|---|---|
| `connect_to(host, port)` | Starts connecting. Returns `OK`, or `ERR_INVALID_PARAMETER` with the reason in `get_last_error()`: no valid schema, a port outside 1 to 65535, or a connection that could not be opened. A server that refuses you, or is not there, shows up later, as the state. |
| `disconnect_from_server()` | Leaves the server. Sends no signal. Harmless if you call it twice or before connecting. |
| `poll()` | Reads what arrived and sends the signals for it. |
| `get_state()` | One of `TesseraClient.State.CONNECTING`, `HANDSHAKING`, `CONNECTED`, `REJECTED`, `DISCONNECTED`. A new client is `DISCONNECTED`. |
| `get_rejection()` | Why the server said no. Empty unless the state is `REJECTED`. |
| `get_last_error()` | The reason the last `connect_to` failed. |
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

## When something goes wrong

- **The schema does not match the server's.** The state becomes `REJECTED`, the `rejected` signal fires and
  `get_rejection()` says that the schema is not the same and tells you to compare types, properties and their order, and
  the step and range of every position. The client never connects quietly with values it would read wrongly.
- **The server is not there.** The connection times out after a few seconds and you get `disconnected`. The same happens
  for a host name that does not exist.
- **The server is full.** The state becomes `REJECTED` and the reason says the server is full.
- **Using the client at the wrong moment.** Every call is safe in every state: before connecting and after
  disconnecting, `get_entities()` is empty, `get_property()` is `null`, and `poll()` does nothing.

## What the client does not do

There is no prediction or reconciliation, no smoothing between updates (an entity jumps from one received position to
the next), no code generation and no shared schema file with the server (you write the description twice), and the
server is not a Godot node. There is no editor plugin yet.

## Limits to know about

- **Entity ids are `int`s with a sign.** The id has 64 bits, and a server with shard 2048 or higher sets the top bit, so
  its ids are negative in GDScript. They are still unique, and `int(id)` is what you pass back to `get_property`.
- **A 64-bit `mask` is a signed `int`**, as described in the table.
- **`entity_appeared` means the state arrived.** An entity the server can see but has not sent yet is not listed.
- **`poll()` cost.** It looks at every entity the client holds, so its cost grows with their number. No measurement of
  it is published.
