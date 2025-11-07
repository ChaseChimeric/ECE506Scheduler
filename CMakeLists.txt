cmake_minimum_required(VERSION 3.14)
project(schedrt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(schedrt
    src/scheduler.cpp
    src/accelerators.cpp
)

target_include_directories(schedrt
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(schedrt PUBLIC Threads::Threads)

add_executable(sched_demo apps/demo_main.cpp)
target_link_libraries(sched_demo PRIVATE schedrt)

