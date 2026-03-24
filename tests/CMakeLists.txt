cmake_minimum_required(VERSION 3.14)
project(note-cpp-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings matching ci.sh
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Werror)

# Include paths
set(NOTE_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
include_directories(${NOTE_CPP_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR})

# Auto-discover all test sources
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(note-cpp-tests test_main.cpp ${TEST_SOURCES})
