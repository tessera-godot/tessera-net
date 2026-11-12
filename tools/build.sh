#!/usr/bin/env bash
# Builds and tests Tessera Net on Linux, Release and then Debug, and runs the 495 tests of the core.
# Usage: ./tools/build.sh
# Requires: CMake 3.21+, Ninja and a C++20 compiler (GCC 11+ or Clang 14+ with libstdc++).
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release

# Debug build: assertions are enabled here, so invariant violations are caught.
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug

echo "Build and tests succeeded."
