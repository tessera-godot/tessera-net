# SPDX-License-Identifier: Apache-2.0
# tessera_schema: reads a schema file, checks it with the same code the server uses, and writes C++ from it.
# Built with -DTESSERA_BUILD_SCHEMA_TOOL=ON, and always with -DTESSERA_BUILD_SERVER=ON (see the top-level
# CMakeLists.txt). The library depends on the core library and nothing else.
add_library(tessera_schema_tool STATIC
    src/cli.cpp
    src/json_reader.cpp
    src/schema_file.cpp
    src/names.cpp
    src/columns.cpp
    src/schema_check.cpp)

target_include_directories(tessera_schema_tool PUBLIC include)
target_link_libraries(tessera_schema_tool PUBLIC tessera_net_core)

add_executable(tessera_schema tool/main.cpp)
target_link_libraries(tessera_schema PRIVATE tessera_schema_tool)

if(MSVC)
    target_compile_options(tessera_schema_tool PRIVATE /W4 /permissive-)
    target_compile_options(tessera_schema PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_schema_tool PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(tessera_schema PRIVATE -Wall -Wextra -Wpedantic)
endif()
