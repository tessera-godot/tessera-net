# Contributing

Thank you for your interest. This project is in an early design phase, so the most helpful contributions
right now are feedback on the design, use cases, and measurements.

## Ground rules

- **Design first.** Larger changes start as an issue or a design note, and become an architecture decision
  record (see `docs/adr/`) when they affect the structure of the project.
- **Small, focused changes.** One concern per pull request.
- **Tests.** Every behavior change comes with automated tests. Network rules (determinism, reconciliation,
  relevancy) are always tested.
- **Measure, do not guess.** Performance numbers in the docs come from the benchmarks in `bench/`. If your
  change affects them, run the benchmark again and update `docs/benchmarks.md` and the charts
  (`python tools/make_charts.py`).

## Code style

- C++20, formatted with the `.clang-format` in the repository.
- Clean Code and SOLID: small units with one responsibility, dependencies through interfaces.
- Comments and identifiers in English. Comment the reason, not the obvious.
- No allocations on the per-tick hot path.
- The `core/` library must never depend on Godot.

## Documentation

Every user-facing feature needs documentation written for newcomers: what it is, how to use it, and a
working example. Keep the language simple and explain technical terms.

## Commit messages

Short, in the imperative mood, in English (for example: `Add entity handle type`).

## Building and testing

See the build section of the [README](README.md). All tests must pass before you open a pull request.

### Running the CI checks locally

The workflow in `.github/workflows/ci.yml` builds and tests with these presets, in Release and Debug, with and without
ENet (`server` is the server library and its tests, `enet` adds the ENet transport and the example server), and it
builds the default configuration, with every option off, on both systems:

```powershell
# Windows (Visual Studio 2022); CXXFLAGS makes a warning in our C++ an error, as in the workflow
$env:CXXFLAGS = "/WX"
cmake --preset windows-msvc-enet -DTESSERA_BUILD_BENCH=OFF
cmake --build --preset windows-msvc-enet-release
ctest --preset windows-msvc-enet-release
```

```bash
# Linux (CMake 3.21+, Ninja, GCC 11+ or Clang 14+ with libstdc++)
export CC=clang CXX=clang++ CXXFLAGS=-Werror
cmake --preset linux-enet-release
cmake --build --preset linux-enet-release
ctest --preset linux-enet-release
```

`CXXFLAGS` is read when CMake first configures a build directory, so set it before the first `cmake --preset`; in a
directory that already exists, delete it or use a new one. In the same way, `-DTESSERA_BUILD_BENCH=OFF` is kept in the
cache of `build/windows-msvc-enet`, and later configures keep it off until you pass `-DTESSERA_BUILD_BENCH=ON`. The
ENet presets need a Python 3 interpreter, which runs the test that sends signals to the example server.

Use `windows-msvc-server-*` or `linux-server-*` for the build without ENet. The CI runs Linux, so a change that touches
code the Linux build compiles is checked there; if you can, try it with both GCC and Clang.
