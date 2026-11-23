# Schema file reference

A schema says which properties each type of entity replicates: where an entity is, which way it faces, how much health it
has. The server and the client have to agree on it, and writing it twice is where mistakes creep in. This page describes
a way to write it once, in a JSON file, and to have both sides made from it by a tool, `tessera_schema`: the C++ of the
server, and a GDScript file for the client in Godot.

The client can also describe the same types by hand with `TesseraSchema.add_type` (see
[Using the client in Godot](../guides/using-the-client-in-godot.md)); that still works, and the generated script does
exactly that for you. Either way the two descriptions have to say the same thing, and the hash of the schema, which the
client sends when it connects, is what tells when they do not.

## An example

This is the file of the example server (`server/example/example_schema.json`):

```json
{
  "format_version": 1,
  "types": [
    {
      "type": 1,
      "name": "entity",
      "note": "The entities that walk around the world.",
      "properties": [
        {"name": "position", "kind": "position", "step": 0.01, "range": 2048.0}
      ]
    },
    {
      "type": 2,
      "name": "player",
      "note": "One for each connected player, with the connection that owns it.",
      "properties": [
        {"name": "position", "kind": "position", "step": 0.01, "range": 2048.0},
        {"name": "owner", "kind": "integer", "min": 0, "max": 65535, "signed": false}
      ]
    }
  ]
}
```

## What goes in the file

| Where | Field | Meaning |
|---|---|---|
| file | `format_version` | The version of this format. It is `1`, and the tool refuses any other number. |
| file | `types` | The list of entity types (at least one). |
| type | `type` | The number of the type, a whole number from 0 to 255. Each type has its own. |
| type | `name` | The name of the type, used to make the names in the C++ (see below). |
| type | `note` | Optional. A short remark, copied into the generated code as a comment. |
| type | `properties` | The list of the replicated properties, in the order they are sent. The order is part of the schema. |
| property | `name` | The name of the property. Two properties of a type cannot share a name. |
| property | `kind` | One of the six kinds below. |
| property | `note` | Optional, like the one of the type. |
| property | `step`, `range` | Kind `position`: the size of a step in world units, and the distance from the origin within which positions are kept. Numbers. |
| property | `bits` | Kind `angle` (1 to 16) and kind `mask` (1 to 64). A whole number. |
| property | `min`, `max`, `signed` | Kind `integer`: the lowest and highest value (whole numbers), and whether the column holds negative numbers (`signed`, `false` when left out). |
| property | `count` | Kind `choice`: how many values there are (at least 2). A whole number. |

A field that is not in the table, a key that belongs to another kind (`bits` on a `flag`), a key a kind needs and does not
have, and a value of the wrong sort (text where a number goes, `1.0` where a whole number goes) are all errors. Notes are
plain ASCII, on one line, up to 200 characters, with no backslash (it would join the next line of the generated code to the comment). These are the same keys and the same kind names that `add_type` takes in
GDScript.

A `step` or a `range` too large for a `float` (above about 3.4e38) or too small to be one (below about 1e-45) is
refused, like any other that the replication code refuses.

The six kinds and the C++ column each one gets:

| Kind | What it holds | C++ column |
|---|---|---|
| `position` | Three floats, sent as small steps. | Three `float` columns, `<name>_x`, `<name>_y` and `<name>_z`. |
| `angle` | A float in radians, sent in `bits` bits. | One `float` column. |
| `integer` | A whole number from `min` to `max`. | The smallest type that holds the range: `int8_t` to `int64_t` when `signed` is true, `uint8_t` to `uint64_t` when not. |
| `flag` | One true or false value. | `uint8_t`. |
| `mask` | Up to 64 flags in one number. | `uint8_t`, `uint16_t`, `uint32_t` or `uint64_t`, by the number of `bits`. |
| `choice` | One of `count` values. | `uint8_t`, `uint16_t` or `uint32_t`, by `count`. |

The size of a column does not change the hash: the hash depends on what the bytes mean, not on how the server stores them.

### Names

The name of a type or of a property is lowercase letters, digits and underscores, each part starting with a letter
(`max_health`, `player_one`), up to 48 characters. Names that are keywords in C++ or in GDScript (or the GDScript literal `null`) are refused, and so are
`min`, `max`, `near`, `far`, `small`, `interface`, `linux` and `unix`, which some C++ headers turn into macros. Two names
that would make the same name in the generated C++ (a property `position` and another called `position_x`, for example)
are an error of the file, and say where.

The limits of the schema itself are the ones of the replication code, because the tool uses that code to check the file:
for example, the replicated state of a type takes at most 32 words (a position takes 3, an integer or a mask 2, an angle
or a choice 1).

## The tool

`tessera_schema` is built together with the server: `-DTESSERA_BUILD_SERVER=ON` builds it, and so does
`-DTESSERA_BUILD_SCHEMA_TOOL=ON` on its own. It is a program of the build folder (on Windows with the Visual Studio
presets, `build/windows-msvc-server/schema/Debug/tessera_schema.exe`).

```text
tessera_schema check <schema.json>
tessera_schema hash <schema.json>
tessera_schema generate [--check] [--cpp <output.hpp> [--namespace a::b]]
                        [--gdscript <output.gd> [--gdscript-class Name]] <schema.json>
```

- `check` reads the file and checks it the way the server does. For the example above it prints
  `server/example/example_schema.json: OK, 2 types, hash 0x04b6ebf2e86202d0`.
- `hash` prints only that hash, `0x04b6ebf2e86202d0`.
- `generate` writes what you ask for: the C++ header with `--cpp`, the GDScript with `--gdscript`, or both in one call
  (give at least one). The namespace of everything in the header is `--namespace` (`game_schema` when left out; it is
  made of valid C++ names, and not `std`, a keyword, a macro such as `linux`, or a name with two underscores), and it
  only goes with `--cpp`. The class of the script is `--gdscript-class` (`GameSchema` when left out), and it only goes
  with `--gdscript`; see "The name of the class" below. The tool checks the schema first, and writes nothing when it is
  wrong; files from an earlier run stay as they were. It makes the folders of an output when they are missing, and does
  not write a file again when the bytes would be the same, so the build does not recompile what includes it for nothing.
  It refuses to write over the schema file itself, and to write the header and the script to one path, and an option
  given twice. The header is written first and then the script: if the script cannot be written, the header already
  has been.
- `generate --check` takes the same arguments and writes nothing, not a file and not a folder. For each output it asks
  for, it says whether the file is what the tool makes from the schema now. Use it to find out that a file you keep in
  the repository has gone out of date (see [In your build](#in-your-build-cmake)).

The exit code is 0 when all went well, 1 when the file or the schema is wrong, 2 when the command line is wrong, and 3 when
a file could not be read or written. With `--check`, 0 means that every output is what the schema makes, and 1 that one
is different or missing (3 wins when an output exists and cannot be read, a folder for example). This is what it says
about a file that is up to date:

```text
examples/godot-client-demo/example_schema.gd: up to date
```

and about one that is not (the path of the file you gave is first, and the command is the one you ran, without
`--check`):

```text
example_schema.gd: error: this file is not what tessera_schema makes from server/example/example_schema.json now
  fix: make the files again with this command:
    tessera_schema generate --gdscript example_schema.gd --gdscript-class ExampleSchema server/example/example_schema.json
```

A file that is only a line ending away (CRLF where the tool writes LF) is reported the same way, with the extra line
`note: only the line endings differ; the tool writes LF (see .gitattributes)`.

Errors go to the error output, one for each problem, with the file, the line and the column, and a correction when there is
an obvious one. This is the output for a file with a few mistakes:

```text
enemies.json:4:25: error: "Enemy" is not a valid name
  fix: use lowercase letters, digits and underscores, and start each part with a letter, for example "max_health"
enemies.json:5:7: error: a property of the kind "position" needs the key "step"
  fix: add "step": a number
enemies.json:5:48: error: unknown field "stp" in a property
  fix: did you mean "step"?
enemies.json:6:7: error: a property of the kind "integer" needs the key "max"
  fix: add "max": a whole number
```

When the file is fine for the reader but the replication code refuses it, the message is the one of the replication code,
with the type in front and the place of the property:

```text
zero.json:2:3: error: type "enemy" (1): property 0: angle bits must be 1 to 16
  fix: "bits" is a whole number from 1 to 16
```

The reader takes standard JSON and nothing more: no comments (use `note`), no trailing commas, no single quotes. It reads
files of up to 1 MiB, nested up to 16 levels, with strings of up to 1,024 bytes.

## The generated header

For each type, in the order of the type numbers (not the order in the file), the header has:

- constants: `k<Type>Type` (the type number) and one for each number of a property, such as `k<Type><Property>Step`,
  `...Range`, `...Bits`, `...Min`, `...Max` and `...Count`;
- `struct <Type>Columns`, the struct of arrays that an `EntityTable` keeps, with one array for each column;
- `Add<Type>Type(schema, error)`, which adds the type to a `ReplicationSchema` (it returns `false`, and says why in `error`
  when you pass one, if the schema refuses it);
- `Make<Type>Table(shard)`, which makes the `EntityTable` of the type with the shard and the type number already in the
  ids it creates. It throws `std::invalid_argument` when the shard does not fit in an id.

At the end there are `AddGeneratedTypes(schema, error)`, which adds every type, and `kSchemaHash`, the hash of the whole
file. A schema made of only some of the types, for example by calling just `AddEntityType`, has a different hash.

The names are made from the names in the file: type `player` gives `PlayerColumns`, `kPlayerType`, `AddPlayerType` and
`MakePlayerTable`, and its property `owner` gives `kPlayerOwnerMax`. Using it looks like this:

```cpp
#include "example_schema.hpp"

net::ReplicationSchema schema;
std::string error;
if (!tessera::server::example::schema::AddGeneratedTypes(schema, &error)) {
    // `error` says which type the replication code refused, and why.
}
auto players = tessera::server::example::schema::MakePlayerTable(context.shard_id);
auto& columns = players->Columns(page);
columns.position_x[slot] = x;
```

The same file always gives the same bytes: lines end in LF, and the header has no date and no path, only the name of the
schema file. The tests generate a header twice and compare, and compare one with a copy kept in the repository.

## The generated GDScript

`generate --gdscript` writes one script, with the types in the order of their numbers. This is the one for the example
file above, made with `--gdscript-class ExampleSchema`; it is `examples/godot-client-demo/example_schema.gd`, which the
demo uses:

```gdscript
# Generated by tessera_schema from example_schema.json. Do not edit: change that file and generate again.
class_name ExampleSchema
extends RefCounted

## The fingerprint of the whole schema file: the number TesseraSchema.get_hash() gives for the schema that
## create() makes. It is a signed 64-bit integer, so it can be negative.
const HASH: int = 339718250426401488

# The entities that walk around the world.
const TYPE_ENTITY := 1
# One for each connected player, with the connection that owns it.
const TYPE_PLAYER := 2


## Describes every type of the file in a new TesseraSchema. A type that TesseraSchema refuses is reported
## with push_error, and the schema is returned anyway, so that the mistake shows where it was made.
static func create() -> TesseraSchema:
	var schema := TesseraSchema.new()
	_add_type(schema, TYPE_ENTITY, "entity", [
		{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0},
	])
	_add_type(schema, TYPE_PLAYER, "player", [
		{"name": "position", "kind": "position", "step": 0.01, "range": 2048.0},
		{"name": "owner", "kind": "integer", "min": 0, "max": 65535, "signed": false},
	])
	return schema


static func _add_type(schema: TesseraSchema, type_id: int, type_name: String, properties: Array) -> void:
	if schema.add_type(type_id, properties) != OK:
		push_error("ExampleSchema: type '%s' (%d) was refused: %s" % [type_name, type_id, schema.get_error()])
```

- `class_name` is the name you gave with `--gdscript-class`, so that other scripts write `ExampleSchema.create()`.
- `TYPE_<NAME>` is the number of each type, with the name in capitals, and the note of the type as a comment above it.
  The server and the client then use the same numbers without typing them twice.
- `HASH` is the hash of the whole file, the same number `TesseraSchema.get_hash()` gives for the schema `create()` makes.
  It is a signed 64-bit integer, so it can be negative: for the file with every kind of property in the tests it is
  `-3382204074482803244`. `tessera_schema hash` prints the same number as an unsigned value in hexadecimal
  (`0xd10fff505e92ddd4` for that file).
- `create()` makes a new `TesseraSchema` each time and calls `add_type` for every type, with the same dictionaries you
  would have written. If `add_type` refuses a type, the script reports it with `push_error` (the class, the type and the
  reason) and still returns the schema, so the mistake shows where it was made. A script made by a newer tool than the
  extension it runs on, with a kind or a key that the extension does not know, is the way that can happen.
- The notes of properties are comments at the end of their line (`# In meters.`). They are never keys, because
  `add_type` refuses a key it does not know.

`step` and `range` are written as the text with the fewest digits that gives the same `float`, which is what the extension turns the
number into. A number from `0.0001` up to `100000000000000.0` is written out (`0.01`, `2048.0`, `100000.0`), and a smaller
or larger one keeps its exponent (`1e-05`, `1e+30`). Integers are written as integers, and the
lowest 64-bit integer as `(-9223372036854775807 - 1)`. The same file always gives the same bytes, in the same way as the
header: lines end in LF, indentation is tabs, and there is no date and no path.

Two things to know before using it:

- The script only loads when the extension is loaded, because `TesseraSchema` is a class of the extension.
- `class_name` is registered when Godot reads the project, and so is the extension: open the project in the editor once,
  or run `godot --headless --path <folder> --import`. Until then a script that writes `ExampleSchema` fails with
  `Identifier "ExampleSchema" not declared in the current scope`.

The script was loaded by Godot 4.7.2, headless, on Windows and on Linux: the hash that `create()` gives was the one in
`HASH`, for the example file and for a file with one property of every kind. That is all that has been tried.

### The name of the class

The class name is a capital letter followed by letters and digits, with at least one lowercase letter (`GameSchema`,
`World2`; not `game_schema`, `GAME` or `PI`), up to 48 characters. It cannot be a name Godot already uses, because
Godot refuses a `class_name` that hides a class, a built-in type or a keyword: the tool knows 1,075 names: the classes,
built-in types and singletons of Godot 4.7.2 (1,071), `Variant`, `GDScriptFunctionState` (a class Godot registers for
itself), and the two classes of this extension, `TesseraSchema` and `TesseraClient`. `Nil` is among them: the tool
refuses it, though Godot may not. When you choose `Node`, it says so and suggests `NodeSchema`.

The list is the one of Godot 4.7.2: a newer Godot may add classes, and a class or an autoload of your own project with
the same name is not known to the tool. Godot has the last word, and it reports the clash when it loads the script.

## In your build (CMake)

`schema/cmake/tessera_schema.cmake` has two functions for a project that brings this repository in as a folder
(`add_subdirectory`) with `TESSERA_BUILD_SCHEMA_TOOL` set to `ON` before it; they only exist when the tool is built. The
[guide](../guides/describe-your-entities-once.md) goes through an example project step by step.

```cmake
tessera_schema_generate(<target> SCHEMA <schema.json>
                        [CPP <header.hpp> [NAMESPACE <a::b>]]
                        [GDSCRIPT <script.gd> [CLASS <Name>]])

tessera_schema_check_test(<test name> SCHEMA <schema.json>
                          [CPP <header.hpp> [NAMESPACE <a::b>]]
                          [GDSCRIPT <script.gd> [CLASS <Name>]])
```

`tessera_schema_generate` makes the files while your project is built; give `CPP`, `GDSCRIPT` or both. A relative path
is read from a different folder for each option:

| Option | In `tessera_schema_generate` | In `tessera_schema_check_test` |
|---|---|---|
| `SCHEMA`, `GDSCRIPT` | the folder of the `CMakeLists.txt` that makes the call | the same |
| `CPP` | the build folder | the folder of the `CMakeLists.txt` |

It makes `<target>`, a library with no code that carries the folder of the header and the core library, so that what
links it can include the header; `<target>_generate`, which writes the header when the schema file changes; and
`<target>_gdscript`, which is part of every build and writes the script each time (the tool does not write a file again
when its bytes would be the same). The script is not the output of a build rule because `clean` deletes the outputs of
build rules, also those in the source folder (seen with the Visual Studio and Ninja generators), and the script is a
file you keep. An option that is wrong, missing or given twice stops the configure with a message that starts with the
name of the call and says how to write it.

`tessera_schema_check_test` registers a test for `ctest` that runs `generate --check` on files you keep in the
repository, and fails with the command that makes them again when they are old. This repository uses it for the script of
the demo (`tessera_schema_demo_check`).

## How the example server uses it

`server/example/example_game.hpp` has no `AddType` of its own. `server/CMakeLists.txt` calls `tessera_schema_generate` to make
`example_schema.hpp` from `server/example/example_schema.json` while the project is built, in
`build/<preset>/generated/server_example/` and not in the source tree, and the example includes it. The hash of the
schema that comes out is the same as the one the example had when it was written by hand: `0x04b6ebf2e86202d0` with
the players (`339718250426401488`), and `1615724867110258213` for the entity type alone. Tests keep both numbers.

The Godot demo takes its description from the same file. `examples/godot-client-demo/example_schema.gd` is kept in the
repository, so that the demo opens in the editor as it is, and a test of the build makes the script again from
`server/example/example_schema.json` and fails when the two differ. After a change in the file, make the script again
from the root of the repository:

```text
tessera_schema generate --gdscript examples/godot-client-demo/example_schema.gd --gdscript-class ExampleSchema server/example/example_schema.json
```

## What does not exist yet

- An installed package: there is no `find_package`. You add this repository as a folder of your project.
- A way to move from one version of a schema to another: the hash only tells that two schemas differ.
- Kinds of property beyond the six above.
