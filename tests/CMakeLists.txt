# Tests use a tiny built-in check macro so the project builds with no third-party dependency.
# A test framework can replace it later (see docs/adr/0003-library-policy.md).
add_executable(tessera_net_tests test_main.cpp test_version.cpp)
target_link_libraries(tessera_net_tests PRIVATE tessera_net_core)

if(MSVC)
    target_compile_options(tessera_net_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_net_tests COMMAND tessera_net_tests)
