find_package(Threads REQUIRED)

add_library(tessera_net_core STATIC
    src/version.cpp
    src/job_system.cpp
    src/spatial_grid.cpp
    src/tick_loop.cpp
    src/relevancy/relevancy_config.cpp
    src/relevancy/compact_ids.cpp
    src/relevancy/relevancy_groups.cpp
    src/relevancy/visible_set.cpp
    src/relevancy/relevancy_engine.cpp
    src/replication/bit_stream.cpp
    src/replication/property_schema.cpp
    src/replication/property_codecs.cpp
    src/replication/state_store.cpp
    src/replication/connection_state.cpp
    src/replication/record_codec.cpp
    src/replication/receiver_state.cpp
    src/replication/replication_engine.cpp
    src/replication/subscription_index.cpp
    src/transport/simulated_network.cpp
    src/protocol.cpp
    src/client_replica.cpp
    src/replication_server.cpp)

target_include_directories(tessera_net_core PUBLIC include)
target_compile_features(tessera_net_core PUBLIC cxx_std_20)
target_link_libraries(tessera_net_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(tessera_net_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
