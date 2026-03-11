cmake_minimum_required(VERSION 3.14)
project(note-cpp-nlohmann-integration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(test_nlohmann_backend test_nlohmann_backend.cpp)
target_include_directories(test_nlohmann_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_link_libraries(test_nlohmann_backend PRIVATE nlohmann_json::nlohmann_json)

enable_testing()
add_test(NAME nlohmann_backend COMMAND test_nlohmann_backend)
