find_package(Threads REQUIRED)

add_library(tessera_net_core STATIC
    src/version.cpp
    src/job_system.cpp)

target_include_directories(tessera_net_core PUBLIC include)
target_compile_features(tessera_net_core PUBLIC cxx_std_20)
target_link_libraries(tessera_net_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(tessera_net_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
