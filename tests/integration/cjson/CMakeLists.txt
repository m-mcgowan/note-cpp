cmake_minimum_required(VERSION 3.14)
cmake_policy(SET CMP0077 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(note-cpp-cjson-integration LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    cJSON
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.18
)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cJSON)

# test_cjson_backend moved to top-level tests/CMakeLists.txt under
# note-cpp-integration-backends. This file retains only the standalone
# alloc-profile target, which uses file-local operator new/delete
# overrides that cannot coexist with the unified doctest binary until
# Phase 2's shared alloc-counter TU lands.

add_executable(test_alloc_profile test_alloc_profile.cpp)
target_include_directories(test_alloc_profile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${cjson_SOURCE_DIR}
)
target_link_libraries(test_alloc_profile PRIVATE cjson)

enable_testing()
add_test(NAME alloc_profile COMMAND test_alloc_profile)
