cmake_minimum_required(VERSION 3.10)
project(untitled)

set(CMAKE_CXX_STANDARD 20)

add_executable(untitled main.cpp executer.h future.h coroutine.h task.h)
add_definitions(-fcoroutines-ts -D_GNU_SOURCE)

find_package(Boost 1.69.0 REQUIRED)
message(includes: ${Boost_INCLUDE_DIR})
INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIR})
