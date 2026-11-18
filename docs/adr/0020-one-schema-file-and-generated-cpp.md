# 0020. One schema file, with the server's C++ generated from it

- **Status:** Proposed
- **Date:** 2026-11-17

## Context
The entities of a game are described twice today: in C++ on the server (`ReplicationSchema::AddType` and a struct of
columns for each type, written by hand) and in GDScript on the client (`TesseraSchema.add_type`). The hello message
carries a hash of the schema, so a client built from a different description is refused, but the mistake only shows when
a client connects, and whoever makes the game writes everything twice.
[0016](0016-the-godot-extension-client-first.md) left this for later.

## Decision
**The schema is written once, in a JSON file.** The file has a `format_version` (1 for now), and a list of types, each
with a number, a name and a list of properties. A property has a name, a kind and the numbers its kind needs. The keys and
the kind names are the ones `TesseraSchema.add_type` already takes in GDScript (`position`, `angle`, `integer`, `flag`,
`mask`, `choice`), so a property is written the same way in both places. A field the reader does not know is an error, and
so is a key that belongs to another kind, because a misspelled key that is ignored changes the schema without a word.
JSON was chosen over a format of our own (one more reader to write and for tools to learn), over Godot's `.tres` (only
Godot reads it, and the server does not use Godot), and over YAML or TOML (a dependency, or a larger reader). It has no
comments, so types and properties can carry a `note`.

**A small strict reader, written for this.** It reads standard JSON and refuses everything else (comments, trailing
commas, single quotes, repeated keys), reads at most 1 MiB, nests at most 16 levels deep and takes strings of at most
1,024 bytes, so a broken or hostile file ends in an error and not in a crash. Every error says where it is, with the line
and the column, and, when there is an obvious correction, what to change. There is no dependency on a JSON library.

**A tool, `tessera_schema`, that checks the file with the code the server uses.** It builds a real `ReplicationSchema`,
so what `AddType` refuses is refused, with the same words, and the hash it prints is `ReplicationSchema::Hash()`:
neither the rules nor the hash are written a second time. It has three commands: `check` (says how many types there are
and the hash), `hash` (prints only the hash) and `generate --cpp <output> [--namespace a::b]`. A schema that does not
check never produces a file, and a file from an earlier run is left alone. Names have to be good identifiers in C++ and in
GDScript (lowercase, digits and underscores; no keyword of either language, and not `null`), and two names that would make the same C++
name are an error of the file, not of the compiler.

**What the file does not describe:** which entities each player sees, the bandwidth budget and how the client predicts
stay in code; so do columns that only the server keeps. The file holds only what both sides have to agree on, which is
what the hash covers.

**The generated header.** For each type, in the order of the type numbers: the columns (a struct of arrays, as
`EntityTable` expects), the constants (type number, step, range, and so on), a function that adds the type to a
`ReplicationSchema`, and a function that makes its table with the shard and the type already in the ids. It also has
`AddGeneratedTypes` and `kSchemaHash`, the hash of the whole file. The table functions depend only on the core library:
the shard is a parameter, and whoever has a `ServerContext` passes `context.shard_id`. The same file always gives the
same bytes (LF line endings, no date, no path), which a test checks by generating twice and comparing.

**The example server uses it.** Its two entity types are in `server/example/example_schema.json`, the header is made while
the project is built, in the build folder, and nothing generated is kept in the repository. The hash of that schema is
the hash of the schema the example had when it was written by hand (the same two numbers, with and without players, are
pinned in tests), so no message and no behavior changed. Building the server builds the tool too; the option
`TESSERA_BUILD_SCHEMA_TOOL` builds it alone.

## Consequences
Whoever makes a game describes the entities once on the server side, gets errors with a place and a correction before the
server starts, and gets the columns and the registration code for free.

The format may change before 1.0, which is why the file says its version and the reader refuses a version it does not
know.

The GDScript side is still written by hand for now: the tool does not generate it yet, so the two descriptions still have
to agree, and the hash still tells when they do not.

Not covered: moving from one version of a schema to another (the hash only says "different"), new kinds of property, and
entity ids that are unique across processes ([0015](0015-preparing-for-server-meshing.md)).

On Windows the tool receives its paths in the local code page, and a path with characters outside ASCII was not tried.
