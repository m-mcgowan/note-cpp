cmake_minimum_required(VERSION 3.14)
project(note-cpp-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Werror)

set(NOTE_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
include_directories(${NOTE_CPP_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR})

# Curated source lists
include(${NOTE_CPP_ROOT}/cmake/note-cpp-sources.cmake)
include(${NOTE_CPP_ROOT}/cmake/note-cpp-generated.cmake)

set(ALL_TEST_SOURCES ${NOTE_CPP_TEST_SOURCES} ${NOTE_CPP_GENERATED_TEST_SOURCES})

# Non-Arduino build (default host environment)
add_executable(note-cpp-tests ${ALL_TEST_SOURCES})
add_test(NAME unit-tests COMMAND note-cpp-tests)

# Arduino build (same sources, ARDUINO defined globally like a real Arduino toolchain).
# Force-include the stubs header so Print/Printable are defined before any note-cpp header.
add_executable(note-cpp-tests-arduino ${ALL_TEST_SOURCES})
target_compile_definitions(note-cpp-tests-arduino PRIVATE ARDUINO=10812 NOTE_ARDUINO_STUBS)
target_compile_options(note-cpp-tests-arduino PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/arduino_stubs.hpp)
add_test(NAME unit-tests-arduino COMMAND note-cpp-tests-arduino)
