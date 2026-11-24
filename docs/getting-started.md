# Getting started

> **Status:** pre-alpha. There is no installable plugin yet. This page describes what you can do today and
> what the first release will look like.

## Today: build and run the tests

You only need this to contribute or to follow development.

1. Install CMake 3.21 or newer.
2. Windows: install Visual Studio 2022 with the *Desktop development with C++* workload.
   Linux: install Ninja and GCC 11+ or Clang 14+.
3. Run the build script from the repository root:

```powershell
./tools/build.ps1      # Windows
```

```bash
./tools/build.sh       # Linux
```

If everything works you will see `Build and tests succeeded.`

## Today: try the Godot client

A first client for Godot exists as a GDExtension. It connects to a Tessera Net server and lets a script read the entities
it sends. You build it from source, together with the example server, and open the demo project. The steps are in
[Using the client in Godot](guides/using-the-client-in-godot.md) and the
[demo](../examples/godot-client-demo/README.md). It has been loaded into Godot 4.7.2 on Windows, and on Linux in a
container.

## Today: describe your entities once

The entities of your game can be described in one JSON file, and the build makes the server's C++ and the client's
GDScript from it:

1. Write `schema.json` (types, and the properties of each).
2. Check it with `tessera_schema check schema.json`.
3. Add this repository as a folder of your project, switch the tool on, and call `tessera_schema_generate` in your
   `CMakeLists.txt`.
4. Include the header in the server and use the generated class in Godot.

The guide [Describe your entities once](guides/describe-your-entities-once.md) goes through it with an example project.

## First release (planned)

1. Download the ZIP from the Releases page.
2. Copy the `addons/tessera_net` folder into your Godot project.
3. Enable the plugin in *Project Settings > Plugins*.
4. A first-run assistant asks what kind of game you are making and applies a preset.
5. Press **Play with Server** to test with a local server and clients.
6. Press **Export Dedicated Server** when you are ready to publish.

You will not need to compile anything to use the released version.
