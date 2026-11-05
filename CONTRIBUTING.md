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
