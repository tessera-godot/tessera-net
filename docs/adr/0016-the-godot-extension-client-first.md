# 0016. The Godot extension: client first

- **Status:** Proposed
- **Date:** 2026-11-12

## Context
The core, the replication, the ENet transport and the server library exist, and nothing lets someone try them from inside
Godot: `extension/` and `addons/` were only notes. Records [0001](0001-engine-independent-core.md) and
[0002](0002-standalone-dedicated-server.md) already say how the pieces fit: the core stays free of Godot, the Godot side
is a GDExtension on top of it, and the server is a separate program.

The first thing to build on the Godot side had to be small enough to finish and to check, and useful enough that a
tester can open a project, connect to a server and see entities move. A client does that. A server inside the editor
would need the game loop, the entity tables and the relevancy settings exposed to scripts, which is a lot of surface to
fix too early.

A client needs to know how the entities of the server are laid out, because the hello message carries a hash of that
layout and the server refuses a client whose layout differs ([0010](0010-transport-and-protocol.md)). Someone has to
describe the entities on the Godot side.

## Decision
**The Godot extension starts with the client.** It has two classes: `TesseraSchema`, a `Resource` that holds the
description of the entity types, and `TesseraClient`, a `RefCounted` that connects, reads and sends signals. The server
stays a program of its own. A project in `examples/godot-client-demo` connects to the example server and draws one sphere
for each entity.

**The client describes the entities in GDScript.** `TesseraSchema.add_type` takes a type number and a list of
dictionaries, one for each property, with a name, a kind (`position`, `angle`, `integer`, `flag`, `mask` or `choice`)
and the numbers that kind needs. There is no code generation and no file shared with the server for now: the same layout
is written once for the server in C++ and once for the client in GDScript. This is the part most likely to be changed
(the schema step of the roadmap is where a shared description belongs). The hash in the hello message is the safety net:
a description that differs from the server's is refused, with a message that says what to compare: the types, the order
of the properties, the kind of each one and its numbers (bits, step and range, min, max, signed, count). The client never
connects quietly with values it would read wrongly. The hash does not include property names, so a client may name things as it likes.

**Everything that does not need Godot lives in its own library**, `tessera_extension_logic`, in the namespace
`tessera::ext`: building a core schema from a description, turning quantized words into values, and `ClientSession`,
which owns a connection, the state it holds and the events a script hears about. It is tested in the ordinary test run
with the simulated network, without Godot. The shared library the editor loads, `tessera_net_godot`, only converts
between `Variant`s and that logic and sends signals.

**ENet is linked into the extension and its symbols are hidden.** Godot has its own copy of ENet, and two copies in one
process can collide if both export the same names. The extension is built with hidden visibility, and on Linux a linker
version script lists the entry point as the only exported name. A test reads the export table of the library (the
`nm` output on Linux, the PE file on Windows) and fails if anything besides the entry point is there. The
alternative we keep in reserve is to use Godot's own `ENetConnection` behind the `ITransport` interface, which would
remove the second copy altogether; it is not needed as long as the first approach holds.

**godot-cpp is fetched at one commit.** The build fetches the commit `27d9dd23c83871e0619fca5dc2cddfbfd69e926a` of the
`4.5` branch. There is no 4.7 branch of godot-cpp to pin, so the
library is built against 4.5 and says in its `.gdextension` file that it needs Godot 4.5 or newer
(`compatibility_minimum`). Godot 4.7.2 loaded it when we tried on Windows. If a later godot-cpp matches the Godot you
use better, changing the commit is one line in `extension/CMakeLists.txt`.

**Four godot-cpp options are set on purpose.** Exceptions stay enabled (the core throws, and godot-cpp would otherwise
define a macro that turns them off for everything it touches), the C++ runtime is not linked statically (the core and
ENet use the shared one, and mixing runtimes does not link), the headers are marked as system headers (so their
warnings are not ours), and symbol visibility is hidden. The target (`template_debug` or `template_release`) is not one
of them: it is chosen when you configure, with one preset for each. On Windows the shared runtime is used in the Debug
build too, so that the library loads on a machine without Visual Studio.

**It is all behind `TESSERA_BUILD_EXTENSION`, off by default.** The option needs `TESSERA_BUILD_ENET` and
`TESSERA_BUILD_SERVER`, because the example server is what the tests and the demo talk to. With it off, nothing is
fetched, no target is added and no global CMake setting changes: the 495 tests are the same.

**The GDScript interface is the first version and is small on purpose.** `TesseraSchema` has `add_type`, `is_valid`,
`get_error`, `get_hash` and a stored `types` property (so a schema can be saved as a resource or copied with
`duplicate()`). `TesseraClient` has `connect_to`, `disconnect_from_server`, `poll`, `get_state`,
`get_rejection`, `get_last_error`, `get_entities`, `get_property`, `get_snapshot`, a `schema` property and five signals.
We make no promise that these stay as they are. Two details a user can trip on are written in the guide: entity ids are
64-bit numbers with a sign (a high shard number makes them negative), and a 64-bit mask comes out as a signed `int`.

**What is left out.** The server as a Godot node, an editor plugin, code generation and a shared schema file, prediction
and reconciliation, smoothing between received states, packaging for the Asset Library, other platforms than Windows and
Linux on x86-64, and any measurement of the extension.

**What has and has not been checked.** On Windows, the library loads in Godot 4.7.2, and a headless Godot connects to
the example server over real sockets, receives its entities and reads their values, and the tests for the odd cases pass
(a bad schema, the wrong schema, a server that is not there, signals that call back into the client, several clients at
once). On Linux, in a container, the code compiles with GCC 13 and Clang 18 without warnings, its tests that need no Godot
pass, the export table is clean, and the same headless test passes against the example server in a Godot 4.7.2 that we
built from source in that container (checked with the Debug library built by each compiler; there is no window and no
graphics in that Godot). Both checks were done locally. The continuous-integration workflow builds the extension on both
systems and has not run yet; it does not start Godot, because we have no Godot binary for Linux that we could pin and
verify.

## Consequences
**Easier:** a tester can use the project from Godot without compiling a server into the editor. The logic is tested
without Godot, which keeps the test run fast and the Godot layer thin. The server and the extension stay independent
programs, which is the shape the earlier records wanted.

**Harder:** the layout of the entities exists in two places, and they can drift apart. The hash catches it and the
message says what to compare, but only when a client connects. Building the extension needs godot-cpp, which is a large
download the first time. The library carries a copy of ENet and of the core, so it is bigger than a thin binding would be.
And because the Linux load was checked only locally, with one Godot build, a problem that only shows on another Linux
setup (a different Godot build, another distribution) is still possible, and the CI workflow would not notice it.

**Open:** whether to replace the second copy of ENet with Godot's, whether to keep godot-cpp 4.5 or move to a newer one,
how to run the Godot test on a Linux runner, and the shared schema.
