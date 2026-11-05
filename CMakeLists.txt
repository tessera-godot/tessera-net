cmake_minimum_required(VERSION 3.21)

project(tessera_net
    VERSION 0.0.1
    DESCRIPTION "High-performance dedicated-server networking for Godot"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TESSERA_BUILD_TESTS "Build the unit tests" ON)

# The core library has no dependency on Godot (see docs/adr/0001-engine-independent-core.md).
add_subdirectory(core)

if(TESSERA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
