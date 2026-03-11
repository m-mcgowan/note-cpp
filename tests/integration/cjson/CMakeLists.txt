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

add_executable(test_cjson_backend test_cjson_backend.cpp)
target_include_directories(test_cjson_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${cjson_SOURCE_DIR}
)
target_link_libraries(test_cjson_backend PRIVATE cjson)

enable_testing()
add_test(NAME cjson_backend COMMAND test_cjson_backend)
