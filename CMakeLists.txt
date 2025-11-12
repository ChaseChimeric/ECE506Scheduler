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
add_library(schedrt SHARED
    src/scheduler.cpp
    src/accelerators.cpp

    # DASH layer sources
    src/dash/provider.cpp
    src/dash/completion_bus.cpp
    src/dash/fft.cpp
    src/dash/zip.cpp
    src/dash/scheduler_binding.cpp
)

set_target_properties(schedrt PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
find_package(ZLIB REQUIRED)
target_link_libraries(schedrt PUBLIC Threads::Threads ZLIB::ZLIB)

# Demos
add_executable(sched_runner apps/sched_runner.cpp)
target_include_directories(sched_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sched_runner PRIVATE schedrt dl)

add_library(demo_dash_app SHARED apps/demo_dash.cpp)
target_include_directories(demo_dash_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(demo_dash_app PRIVATE schedrt)

add_library(radar_correlator_app SHARED apps/radar_correlator.cpp)
target_include_directories(radar_correlator_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(radar_correlator_app PRIVATE schedrt)
