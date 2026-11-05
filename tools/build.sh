#!/usr/bin/env bash
# Builds and tests Tessera Net on Linux. Usage: ./tools/build.sh
# Requires: CMake 3.21+, Ninja and a C++20 compiler (GCC 11+ or Clang 14+).
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release

echo "Build and tests succeeded."
