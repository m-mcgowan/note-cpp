cmake_minimum_required(VERSION 3.14)
project(note-cpp
    VERSION 0.2.0
    LANGUAGES CXX
    DESCRIPTION "Typed C++ SDK for the Blues Notecard"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# `include/` is a symlink to `src/` (the compat alias CMake consumers use).
# It is deliberately kept out of version control — the Arduino Library Manager
# rejects repositories containing symlinks (arduino-lint LS005) — so recreate
# it on demand here. The Arduino build never needs it; it compiles src/
# directly. On platforms without symlink support the fallback copies src/.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include")
    file(CREATE_LINK "src" "${CMAKE_CURRENT_SOURCE_DIR}/include"
         SYMBOLIC COPY_ON_ERROR)
endif()

# Source lists
include(cmake/note-cpp-sources.cmake)
include(cmake/note-cpp-generated.cmake)

# Header-only library target
add_library(note-cpp INTERFACE)
target_include_directories(note-cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(note-cpp INTERFACE cxx_std_17)

# Tests (optional, only when building this project directly)
option(NOTE_CPP_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
if(NOTE_CPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
