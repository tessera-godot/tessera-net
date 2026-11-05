# 0003. Library policy

- **Status:** Accepted
- **Date:** 2026-11-05

## Context
Mature C++ libraries exist for many needs, but every dependency makes building harder for users.

## Decision
Use mature, replaceable libraries for commodity needs (transport, compression, job scheduling, testing,
deterministic physics). Build the replication, relevancy and entity storage ourselves.
A library is adopted only if it has a permissive license, builds through CMake without manual
installation, has few dependencies, sits behind an interface of ours, and (if on the hot path) has been
benchmarked.

## Consequences
- Users can build the project with one command.
- Dependencies can be swapped without rewriting the project.
- The tiny built-in test runner is a placeholder until a framework is adopted under this policy.
