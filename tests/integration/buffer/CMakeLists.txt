cmake_minimum_required(VERSION 3.14)
project(note-cpp-buffer-integration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_buffer_backend test_buffer_backend.cpp)
target_include_directories(test_buffer_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

enable_testing()
add_test(NAME buffer_backend COMMAND test_buffer_backend)
