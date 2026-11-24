# SPDX-License-Identifier: Apache-2.0
# A small game server project that describes its entities in schema.json and gets the C++ and the GDScript from it.
# To use the same lines in your own project, copy this file and change the paths.
cmake_minimum_required(VERSION 3.21)
project(game_schema_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

# This repository, as a folder of the project: nothing is downloaded. Here it is two folders up; in your project it is
# wherever you put it (a git submodule, for example).
set(TESSERA_NET_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "The folder of the tessera-net repository")
# The schema tool has to be built for the function below to exist.
set(TESSERA_BUILD_SCHEMA_TOOL ON CACHE BOOL "" FORCE)
add_subdirectory("${TESSERA_NET_DIR}" tessera-net)

# The header goes in the build folder (a relative CPP is read from there). The script goes in the folder of the Godot
# project, next to this file (a relative GDSCRIPT is read from the folder of this CMakeLists.txt), and is kept in the
# repository: it is written again when the schema changes, and left alone when it does not.
tessera_schema_generate(game_schema
                        SCHEMA schema.json
                        CPP generated/game_schema.hpp
                        NAMESPACE game
                        GDSCRIPT godot/game_schema.gd
                        CLASS GameSchema)

add_executable(game_schema_example main.cpp)
target_link_libraries(game_schema_example PRIVATE game_schema)
if(MSVC)
    target_compile_options(game_schema_example PRIVATE /W4 /permissive-)
else()
    target_compile_options(game_schema_example PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME game_schema_example COMMAND game_schema_example)
