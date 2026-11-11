cmake_minimum_required(VERSION 3.21)

project(tessera_net
    VERSION 0.0.1
    DESCRIPTION "High-performance dedicated-server networking for Godot"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TESSERA_BUILD_TESTS "Build the unit tests" ON)
option(TESSERA_BUILD_BENCH "Build the acceptance benchmark" OFF)
option(TESSERA_PROFILE "Time the stages of the network phase (measurement builds only)" OFF)
option(TESSERA_BUILD_ECS_COMPARISON "Build the comparison with a general-purpose ECS (downloads EnTT)" OFF)
option(TESSERA_BUILD_ENET "Build the ENet transport, its tests and the head-to-head programs (downloads ENet)" OFF)

# The core library has no dependency on Godot (see docs/adr/0001-engine-independent-core.md).
add_subdirectory(core)

if(TESSERA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TESSERA_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(TESSERA_BUILD_ECS_COMPARISON)
    add_subdirectory(bench/ecs-comparison)
endif()

if(TESSERA_BUILD_ENET)
    add_subdirectory(bench/head-to-head)
endif()
