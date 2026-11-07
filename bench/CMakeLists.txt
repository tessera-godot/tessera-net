# Acceptance benchmark: checks the data core against its limits (see docs/benchmarks.md). Enabled with
# -DTESSERA_BUILD_BENCH=ON; it is not part of the default build.
add_executable(tessera_net_bench bench_acceptance.cpp bench_relevancy.cpp bench_replication.cpp
    bench_network.cpp)
target_link_libraries(tessera_net_bench PRIVATE tessera_net_core)
target_include_directories(tessera_net_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(tessera_net_bench PRIVATE psapi)
endif()
target_include_directories(tessera_net_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
if(MSVC)
    target_compile_options(tessera_net_bench PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()
