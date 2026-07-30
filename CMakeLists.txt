cmake_minimum_required(VERSION 3.20)
project(tier_fighter CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The simulation library: pure, deterministic, no dependencies -- not even a
# renderer. Anything that can't be compiled into a headless server does not
# belong in here.
add_library(tf_sim STATIC
    src/sim/sim.cpp
    src/sim/arena.cpp
    src/sim/trig.cpp
)
target_include_directories(tf_sim PUBLIC src)

# -ffast-math is BANNED for the simulation: it permits the compiler to reorder
# and approximate arithmetic, which breaks bit-identical determinism and so
# breaks rollback netcode. The sim uses no floats today, which makes this mostly
# belt-and-braces -- but the flag must never creep in later.
target_compile_options(tf_sim PRIVATE
    -Wall -Wextra -Wpedantic
    -fno-fast-math
    -ffp-contract=off
)

# Determinism and mechanics tests. These are the real safety net: determinism
# cannot be eyeballed, it has to be asserted.
add_executable(tf_tests tests/test_sim.cpp)
target_link_libraries(tf_tests PRIVATE tf_sim)
target_compile_options(tf_tests PRIVATE -Wall -Wextra)

# Arena tests are a separate binary: the simulation and the matchmaking layer fail
# for different reasons and are worth running independently.
add_executable(tf_arena_tests tests/test_arena.cpp)
target_link_libraries(tf_arena_tests PRIVATE tf_sim)
target_compile_options(tf_arena_tests PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME sim_tests COMMAND tf_tests)
add_test(NAME arena_tests COMMAND tf_arena_tests)

# Local playtest client. The ONLY target that links a graphics library -- tf_sim
# stays dependency-free so it compiles into a headless server unchanged.
find_package(raylib QUIET)
if(NOT raylib_FOUND)
    find_library(RAYLIB_LIB raylib HINTS /opt/homebrew/lib /usr/local/lib)
    find_path(RAYLIB_INCLUDE raylib.h HINTS /opt/homebrew/include /usr/local/include)
endif()

if(raylib_FOUND OR (RAYLIB_LIB AND RAYLIB_INCLUDE))
    add_executable(tf_play src/render/main.cpp)
    target_link_libraries(tf_play PRIVATE tf_sim)
    target_compile_options(tf_play PRIVATE -Wall -Wextra)

    if(raylib_FOUND)
        target_link_libraries(tf_play PRIVATE raylib)
    else()
        target_include_directories(tf_play PRIVATE ${RAYLIB_INCLUDE})
        target_link_libraries(tf_play PRIVATE ${RAYLIB_LIB})
    endif()

    if(APPLE)
        target_link_libraries(tf_play PRIVATE
            "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
    endif()
else()
    message(WARNING "raylib not found -- skipping tf_play. Tests still build.")
endif()
