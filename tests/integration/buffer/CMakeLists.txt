cmake_minimum_required(VERSION 3.14)
project(note-cpp-buffer-integration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# test_buffer_backend moved to top-level tests/CMakeLists.txt under
# note-cpp-integration-backends. The alloc-profile + sax-parser targets
# keep their standalone builds — they use file-local operator new/delete
# overrides that cannot coexist with the unified doctest binary until
# Phase 2's shared alloc-counter TU lands.

add_executable(test_sax_parser test_sax_parser.cpp)
target_include_directories(test_sax_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

add_executable(test_alloc_profile test_alloc_profile.cpp)
target_include_directories(test_alloc_profile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

add_executable(test_sax_alloc_profile test_sax_alloc_profile.cpp)
target_include_directories(test_sax_alloc_profile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

enable_testing()
add_test(NAME sax_parser COMMAND test_sax_parser)
add_test(NAME alloc_profile COMMAND test_alloc_profile)
add_test(NAME sax_alloc_profile COMMAND test_sax_alloc_profile)
