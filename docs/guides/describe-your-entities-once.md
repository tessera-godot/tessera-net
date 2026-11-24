# Describe your entities once

Your server and your Godot client both have to know what an entity is: where it stands, how much health it has, which
loot a crate holds. If you write that twice, once in C++ and once in GDScript, the two copies drift apart and the first
sign is a client that reads wrong values. Here you write it once, in a JSON file, and your build makes the C++ for the
server and the GDScript for the client from it. When the file changes, both change together.

This page goes through the small project in [`examples/game-schema`](../../examples/game-schema/README.md), which is built
and tested with the rest of the repository, so what you read here is what runs.

## What you need

- CMake 3.21 or newer and a C++20 compiler.
- This repository in a folder of your project (a git submodule or a copy). It is not installed anywhere, and there is no
  `find_package` for it: your `CMakeLists.txt` adds the folder.
- For the client, Godot 4.5 or newer with the extension of this repository loaded (see
  [Using the client in Godot](using-the-client-in-godot.md)).

## 1. Write the schema file

The file lists your entity types, and for each one the properties that are sent to the clients, in the order they are
sent. This is the whole file of the example, with a monster and a crate:

<!-- source: examples/game-schema/schema.json -->
```json
{
  "format_version": 1,
  "types": [
    {
      "type": 1,
      "name": "monster",
      "note": "Walks around and attacks players.",
      "properties": [
        {"name": "position", "kind": "position", "step": 0.01, "range": 1000.0, "note": "In meters."},
        {"name": "health", "kind": "integer", "min": 0, "max": 100, "signed": false},
        {"name": "enraged", "kind": "flag"}
      ]
    },
    {
      "type": 2,
      "name": "crate",
      "note": "Stays where it is, and holds one kind of loot.",
      "properties": [
        {"name": "position", "kind": "position", "step": 0.01, "range": 1000.0},
        {"name": "loot", "kind": "choice", "count": 4, "note": "Which of four kinds of loot."}
      ]
    }
  ]
}
```

Each type has a number (the same number on the server and in the client), a name and its properties. A property has a
name and one of six kinds: `position`, `angle`, `integer`, `flag`, `mask` and `choice`, each with the numbers it needs
(a position needs a `step` and a `range`, an integer a `min` and a `max`). A `note` is a remark that is copied into
the generated code as a comment. The [schema file reference](../reference/schema-file.md) lists every field, the rules
for names and the limits.

## 2. Check it

The tool `tessera_schema` is built together with the server (and with `-DTESSERA_BUILD_SCHEMA_TOOL=ON` on its own). It
reads your file the way the server does and says what is wrong, with the line and a suggestion, before you build anything:

<!-- shell -->
```text
tessera_schema check examples/game-schema/schema.json
```

When the file is fine it says how many types there are and the hash of the schema:

<!-- output: check examples/game-schema/schema.json -->
```text
examples/game-schema/schema.json: OK, 2 types, hash 0x3b095e13c24c1234
```

## 3. Generate in your build

Two lines of your `CMakeLists.txt` bring in this repository and switch the tool on, and one call asks for the files:

<!-- source: examples/game-schema/CMakeLists.txt -->
```cmake
set(TESSERA_BUILD_SCHEMA_TOOL ON CACHE BOOL "" FORCE)
add_subdirectory("${TESSERA_NET_DIR}" tessera-net)
...
tessera_schema_generate(game_schema
                        SCHEMA schema.json
                        CPP generated/game_schema.hpp
                        NAMESPACE game
                        GDSCRIPT godot/game_schema.gd
                        CLASS GameSchema)
```

The function needs the tool, so the first line has to come before the call. `SCHEMA` is your file. `CPP` is the header
for the server, and `NAMESPACE` is the C++ namespace of what is in it. `GDSCRIPT` is the script for the client, and
`CLASS` is its `class_name`. Give `CPP`, `GDSCRIPT` or both; `NAMESPACE` goes with `CPP` and `CLASS` with `GDSCRIPT`, and
each can be left out to take the tool's default. A call that is wrong stops the configure with a message that says
what to change.

A relative path means a different folder for each option, because the files are different:

| Option | A relative path is read from | Why |
|---|---|---|
| `SCHEMA` | the folder of your `CMakeLists.txt` | the schema file is yours, and is kept in the repository |
| `GDSCRIPT` | the folder of your `CMakeLists.txt` | the script belongs in the folder of your Godot project |
| `CPP` | the build folder | the header is made by the build, and is not kept |

The call makes three things:

- `game_schema`, a library with no code of its own that carries the folder of the header and the core library. Link it to
  what includes the header.
- `game_schema_generate`, which writes the header when the schema file changes.
- `game_schema_gdscript`, which writes the script every time you build. The tool does not write a file again when the
  bytes would be the same, so Godot does not import the script again for nothing. It is a target of its own, and not an
  output of a build rule, because `clean` deletes the outputs of build rules, and your script is a file you keep.

## 4. Use it in the server

Link `game_schema` to your server and include the header. The code it makes has one table of columns for each type,
a function that adds the types to a `ReplicationSchema`, and a function that makes the table of a type:

<!-- source: examples/game-schema/main.cpp -->
```cpp
#include "game_schema.hpp"
...
int main() {
    tessera::net::ReplicationSchema schema;
    std::string error;
    if (!game::AddGeneratedTypes(schema, &error)) {
        std::fprintf(stderr, "the schema was refused: %s\n", error.c_str());
        return 1;
    }
...
    const auto monsters = game::MakeMonsterTable(1);
    const tessera::net::EntityId id = monsters->Create();
    const auto where = monsters->Find(id);
    auto& columns = monsters->Columns(where->page);
    columns.health[where->slot] = 100;
    monsters->MarkDirty(id);
```

The names come from your file: the type `monster` gives `MakeMonsterTable` and `MonsterColumns`, and its property
`health` gives the column `health`. The [schema file reference](../reference/schema-file.md#the-generated-header) lists
all of them.

## 5. Use it in Godot

The script is written into the folder of your Godot project (`godot/` in the example). Open the project once in the
editor, or run `godot --headless --path <folder> --import`: Godot reads the `class_name` and loads the extension then,
and until it has, a script that writes `GameSchema` fails with `Identifier "GameSchema" not declared in the current
scope`. After that the client takes its description from the class (the demo of this repository does the same with
its own class, `ExampleSchema`):

<!-- source: examples/godot-client-demo/main.gd -->
```gdscript
	client.schema = ExampleSchema.create()
```

This is what the script of the example has, in short:

<!-- source: examples/game-schema/godot/game_schema.gd -->
```gdscript
class_name GameSchema
extends RefCounted
...
const HASH: int = 4254034761985298996
...
const TYPE_MONSTER := 1
...
const TYPE_CRATE := 2
```

`TYPE_MONSTER` and `TYPE_CRATE` are the type numbers of your file, `create()` makes the `TesseraSchema` with every type
described, and `HASH` is the fingerprint of the whole file. It is a signed 64-bit integer, so it can be negative. The
script only loads when the extension is loaded, because `TesseraSchema` is a class of the extension.

## When the hash does not match

Both sides compute the same fingerprint from the same file: `kSchemaHash` in the header, `HASH` in the script. The client
sends it when it connects, and a server whose schema has another one refuses the client. The client goes to the state
`REJECTED`, the `rejected` signal fires, and `get_rejection()` says that the schema is not the same and what to compare
(see [When something goes wrong](using-the-client-in-godot.md#when-something-goes-wrong)).

To see which side is old, ask the tool for the hash of the file you have and compare it with the other two. The tool
prints it as an unsigned number in hexadecimal, and the script holds the same number as a signed integer, so a hash
that starts with `8` or higher shows up negative in GDScript:

<!-- shell -->
```text
tessera_schema hash examples/game-schema/schema.json
```

<!-- output: hash examples/game-schema/schema.json -->
```text
0x3b095e13c24c1234
```

## Keep the committed files current

There are two ways to work, and you can pick one for each file.

**The build writes the file and you commit it.** `GDSCRIPT` is written on every build, so after a change in the schema
file the script in your repository is changed by the build, and the change shows up in `git status`. In your own CI,
build and then check that nothing is left changed:

<!-- shell -->
```text
git diff --exit-code
```

**The file is yours and a test guards it.** `tessera_schema_check_test` registers a test for `ctest` that compares a file
you keep with what the tool makes from the schema now, and fails when they differ. It writes nothing. This repository
uses it for the script of its demo:

<!-- source: tests/schema/CMakeLists.txt -->
```cmake
tessera_schema_check_test(tessera_schema_demo_check
                          SCHEMA ${PROJECT_SOURCE_DIR}/server/example/example_schema.json
                          GDSCRIPT ${PROJECT_SOURCE_DIR}/examples/godot-client-demo/example_schema.gd
                          CLASS ExampleSchema)
```

It takes the same options as `tessera_schema_generate`, and every relative path is read from the folder of your
`CMakeLists.txt`, since the files are yours. Use it for a file that your build does not write, because a build that
writes the file first would always leave it in agreement.

The same check by hand is `generate --check` with the arguments you would give to `generate`:

<!-- output: generate --check --gdscript examples/game-schema/godot/game_schema.gd --gdscript-class GameSchema examples/game-schema/schema.json -->
```text
examples/game-schema/godot/game_schema.gd: up to date
```

The exit code is 0 when every file is what the schema makes, 1 when one is different or missing (the message says which,
and prints the command that makes them again), 2 for a wrong command line, and 3 for a file that cannot be read.

One more thing for the files you keep: the tool always writes lines that end in LF, so tell git not to turn them into
CRLF on Windows. This is the line of the example in `.gitattributes`:

<!-- source: .gitattributes -->
```text
examples/game-schema/godot/game_schema.gd text eol=lf
```

Without it, a checkout with `core.autocrlf` would make the script differ from what the tool writes, and the check would
say so, with a note that only the line endings differ.

## What has been tried

The example was built as a project of its own, with the repository added as a folder, on Windows with Visual Studio 2022,
and in an Ubuntu 24.04 container with GCC 13 and with Clang 18, using Ninja. In each of them the script it writes was the
same as the one in the repository, and the check said the files were what the schema makes, before and after a change
in the schema file. The script of the demo was loaded by a headless Godot 4.7.2, as described in
[Using the client in Godot](using-the-client-in-godot.md). All of it was tried on one machine; other generators and
compilers have not been tried.

## What does not exist yet

- Installing the library, or `find_package`: you add this repository as a folder of your project.
- Binaries to download for the tool: you build it, with the project.
- A way to move from one version of a schema to another. The hash only tells that two schemas differ.
- A way to make the ids of entities unique across several server processes.
- Kinds of property beyond the six above.
