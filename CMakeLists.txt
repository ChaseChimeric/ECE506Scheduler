cmake_minimum_required(VERSION 3.14)
project(schedrt LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(BUILD_DEMO "Build simple demo (demo_main)" ON)
option(BUILD_DEMO_DASH "Build DASH-style demo (demo_dash)" ON)

# Library
add_library(schedrt
    src/scheduler.cpp
    src/accelerators.cpp

    # DASH layer sources
    src/dash/provider.cpp
    src/dash/completion_bus.cpp
    src/dash/fft.cpp
    src/dash/zip.cpp
)

# Public includes for consumers (your apps)
target_include_directories(schedrt
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Warnings (optional but recommended)
if (MSVC)
    target_compile_options(schedrt PRIVATE /W4 /permissive-)
else()
    target_compile_options(schedrt PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Threads
find_package(Threads REQUIRED)
target_link_libraries(schedrt PUBLIC Threads::Threads)

# Demos
if (BUILD_DEMO)
    add_executable(sched_demo apps/demo_main.cpp)
    target_link_libraries(sched_demo PRIVATE schedrt)
endif()

if (BUILD_DEMO_DASH)
    add_executable(sched_demo_dash apps/demo_dash.cpp)
    target_link_libraries(sched_demo_dash PRIVATE schedrt)
endif()

