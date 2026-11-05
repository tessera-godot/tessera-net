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

## First release (planned)

1. Download the ZIP from the Releases page.
2. Copy the `addons/tessera_net` folder into your Godot project.
3. Enable the plugin in *Project Settings > Plugins*.
4. A first-run assistant asks what kind of game you are making and applies a preset.
5. Press **Play with Server** to test with a local server and clients.
6. Press **Export Dedicated Server** when you are ready to publish.

You will not need to compile anything to use the released version.
