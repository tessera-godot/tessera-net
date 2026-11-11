# Tests use a tiny built-in check macro so the project builds with no third-party dependency.
# A test framework can replace it later (see docs/adr/0003-library-policy.md).
# Every tests/test_*.cpp file is picked up automatically.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(tessera_net_tests ${TEST_SOURCES})
target_link_libraries(tessera_net_tests PRIVATE tessera_net_core)

if(MSVC)
    target_compile_options(tessera_net_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_net_tests COMMAND tessera_net_tests)

# The ENet transport has its own test executable, so that the main one is the same with or without ENet.
if(TESSERA_BUILD_ENET)
    add_subdirectory(enet)
endif()
