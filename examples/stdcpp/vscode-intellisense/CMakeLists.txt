cmake_minimum_required(VERSION 3.16)
project(note-cpp-intellisense-demo CXX)

# Generates compile_commands.json so clangd / Microsoft C/C++ pick up
# the include paths, defines, and -std flag automatically.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Resolve the note-cpp include tree relative to this CMakeLists.txt
# (../../.. → repo root).
set(NOTE_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

add_executable(demo main.cpp)
target_include_directories(demo PRIVATE "${NOTE_CPP_ROOT}/include")

# Quality-of-life: make warnings visible while you experiment.
target_compile_options(demo PRIVATE -Wall -Wextra -Wpedantic)
