# SPDX-License-Identifier: Apache-2.0
# Tests of the schema tool. Built when the tool is (see tests/CMakeLists.txt). They have their own executable, so that
# the main test program is the same with or without the tool.
file(GLOB SCHEMA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(tessera_schema_tests ${SCHEMA_TEST_SOURCES} ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
target_link_libraries(tessera_schema_tests PRIVATE tessera_schema_tool)
target_include_directories(tessera_schema_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

if(MSVC)
    target_compile_options(tessera_schema_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_schema_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_schema_tests COMMAND tessera_schema_tests)
