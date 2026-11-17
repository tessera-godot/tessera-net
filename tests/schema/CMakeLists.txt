# SPDX-License-Identifier: Apache-2.0
# Tests of the schema tool. Built when the tool is (see tests/CMakeLists.txt). They have their own executable, so that
# the main test program is the same with or without the tool.
file(GLOB SCHEMA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
# schema_test_guard.cpp makes a test that dies end with a message on the console instead of a dialog on MSVC.
add_executable(tessera_schema_tests ${SCHEMA_TEST_SOURCES} schema_test_guard.cpp
               ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
# The header that the tool writes from a schema file of the tests, included by two of the test files.
tessera_schema_generate_cpp(tessera_all_kinds_schema
                            SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/all_kinds.json
                            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/all_kinds.hpp
                            NAMESPACE all_kinds)

target_link_libraries(tessera_schema_tests PRIVATE tessera_schema_tool tessera_all_kinds_schema)
target_include_directories(tessera_schema_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(tessera_schema_tests PRIVATE TESSERA_SCHEMA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(tessera_schema_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_schema_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_schema_tests COMMAND tessera_schema_tests)
