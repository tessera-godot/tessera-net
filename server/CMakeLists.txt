# The production server as a library (configuration, tick loop, shutdown, log). Built only with
# -DTESSERA_BUILD_SERVER=ON (see the top-level CMakeLists.txt). It depends on the core library and nothing
# else.
add_library(tessera_server STATIC
    src/log.cpp
    src/server_config.cpp
    src/server_host.cpp)

target_include_directories(tessera_server PUBLIC include)
target_link_libraries(tessera_server PUBLIC tessera_net_core)

if(MSVC)
    target_compile_options(tessera_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The example server: a small game over the ENet transport, and the model for writing your own.
if(TESSERA_BUILD_ENET)
    add_executable(tessera_example_server example/main.cpp example/stop_signals.cpp)
    target_link_libraries(tessera_example_server PRIVATE tessera_server tessera_net_enet)
    target_include_directories(tessera_example_server PRIVATE example)
    if(MSVC)
        target_compile_options(tessera_example_server PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tessera_example_server PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
