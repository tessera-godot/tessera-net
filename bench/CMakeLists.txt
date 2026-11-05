# Acceptance benchmark: checks the data core against its limits (see docs/benchmarks.md). Enabled with
# -DTESSERA_BUILD_BENCH=ON; it is not part of the default build.
add_executable(tessera_net_bench bench_acceptance.cpp)
target_link_libraries(tessera_net_bench PRIVATE tessera_net_core)
target_include_directories(tessera_net_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
if(MSVC)
    target_compile_options(tessera_net_bench PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()
