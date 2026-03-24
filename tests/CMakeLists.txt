cmake_minimum_required(VERSION 3.14)
project(note-cpp-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Werror)

set(NOTE_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
include_directories(${NOTE_CPP_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR})

# Curated source lists (filenames only — all in this directory)
include(${NOTE_CPP_ROOT}/cmake/note-cpp-sources.cmake)
include(${NOTE_CPP_ROOT}/cmake/note-cpp-generated.cmake)

# Remove arduino_printable from the main test list — it defines ARDUINO
# globally which causes ODR violations when linked with non-ARDUINO TUs.
list(REMOVE_ITEM NOTE_CPP_TEST_SOURCES test_arduino_printable.cpp)

add_executable(note-cpp-tests
    ${NOTE_CPP_TEST_SOURCES}
    ${NOTE_CPP_GENERATED_TEST_SOURCES}
)

# Arduino Printable tests run in a separate binary (defines ARDUINO)
add_executable(note-cpp-tests-arduino test_main.cpp test_arduino_printable.cpp)
add_test(NAME arduino-printable COMMAND note-cpp-tests-arduino)
add_test(NAME unit-tests COMMAND note-cpp-tests)
