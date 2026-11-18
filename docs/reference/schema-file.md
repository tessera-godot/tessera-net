# Schema file reference

A schema says which properties each type of entity replicates: where an entity is, which way it faces, how much health it
has. The server and the client have to agree on it, and writing it twice is where mistakes creep in. This page describes
a way to write it once, in a JSON file, and to have the C++ of the server made from it by a tool, `tessera_schema`.

Right now only the C++ side is generated. The client in Godot still describes the same types in GDScript with
`TesseraSchema.add_type` (see [Using the client in Godot](../guides/using-the-client-in-godot.md)). The two descriptions
have to say the same thing, and the hash of the schema, which the client sends when it connects, is what tells when they
do not.

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
(`max_health`, `player_one`), up to 48 characters. Names that are keywords in C++ or in GDScript are refused, and so are
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
tessera_schema generate --cpp <output.hpp> [--namespace a::b] <schema.json>
```

- `check` reads the file and checks it the way the server does. For the example above it prints
  `server/example/example_schema.json: OK, 2 types, hash 0x04b6ebf2e86202d0`.
- `hash` prints only that hash, `0x04b6ebf2e86202d0`.
- `generate --cpp` writes the C++ header. The namespace of everything in it is `--namespace` (`game_schema` when left
  out). The tool checks the schema first, and writes nothing when it is wrong; a file from an earlier run stays as it was.
  It makes the folders of the output when they are missing, and does not write the file again when the bytes would be the
  same, so the build does not recompile what includes it for nothing. It refuses to write over the schema file itself.

The exit code is 0 when all went well, 1 when the file or the schema is wrong, 2 when the command line is wrong, and 3 when
a file could not be read or written.

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

## How the example server uses it

`server/example/example_game.hpp` has no `AddType` of its own. `server/CMakeLists.txt` makes
`example_schema.hpp` from `server/example/example_schema.json` while the project is built, in
`build/<preset>/generated/server_example/` and not in the source tree, and the example includes it. The hash of the
schema that comes out is the same as the one the example had when it was written by hand: `0x04b6ebf2e86202d0` with
the players (`339718250426401488`), and `1615724867110258213` for the entity type alone. Tests keep both numbers.

## What does not exist yet

- The GDScript side: the client still has to describe the same types in GDScript.
- A way to move from one version of a schema to another: the hash only tells that two schemas differ.
- Kinds of property beyond the six above.
