# The production server as a library (configuration, tick loop, shutdown, log). Built only with
# -DTESSERA_BUILD_SERVER=ON (see the top-level CMakeLists.txt). It depends on the core library and nothing else.
add_library(tessera_server STATIC
    src/log.cpp
    src/server_config.cpp)

target_include_directories(tessera_server PUBLIC include)
target_link_libraries(tessera_server PUBLIC tessera_net_core)

if(MSVC)
    target_compile_options(tessera_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_server PRIVATE -Wall -Wextra -Wpedantic)
endif()
