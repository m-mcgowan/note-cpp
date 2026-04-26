cmake_minimum_required(VERSION 3.14)
project(note-cpp-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wnon-virtual-dtor
    -Wold-style-cast -Wextra-semi -Wsuggest-override
    -Wzero-as-null-pointer-constant -Wimplicit-fallthrough
    -Werror
)

set(NOTE_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
include_directories(${NOTE_CPP_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR})

# Curated source lists
include(${NOTE_CPP_ROOT}/cmake/note-cpp-sources.cmake)
include(${NOTE_CPP_ROOT}/cmake/note-cpp-generated.cmake)

# Detect minimal builds: NOTE_MINIMAL implies NOTE_NO_POLYMORPHIC, which
# strips the polymorphic Notecard class and the tests that depend on it.
# Check CMAKE_CXX_FLAGS for either flag (passed via -DCMAKE_CXX_FLAGS="-DNOTE_MINIMAL").
string(FIND "${CMAKE_CXX_FLAGS}" "NOTE_MINIMAL" _minimal_pos)
string(FIND "${CMAKE_CXX_FLAGS}" "NOTE_NO_POLYMORPHIC" _no_poly_pos)
if(_minimal_pos GREATER -1 OR _no_poly_pos GREATER -1)
    set(ALL_TEST_SOURCES ${NOTE_CPP_TEST_SOURCES_COMMON})
else()
    set(ALL_TEST_SOURCES ${NOTE_CPP_TEST_SOURCES} ${NOTE_CPP_GENERATED_TEST_SOURCES})
endif()

# Non-Arduino build (default host environment)
add_executable(note-cpp-tests
    ${NOTE_CPP_ROOT}/tests/doctest_main.cpp
    ${NOTE_CPP_ROOT}/tests/common/alloc_counter.cpp
    ${ALL_TEST_SOURCES}
)
add_test(NAME unit-tests COMMAND note-cpp-tests)

# Compile-fail tests: verify that invalid API usage doesn't compile.
# Each .cpp file should fail to compile. The test passes if compilation fails.
file(GLOB COMPILE_FAIL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/*.cpp")
foreach(src ${COMPILE_FAIL_SOURCES})
    get_filename_component(test_name ${src} NAME_WE)
    add_test(
        NAME "compile-fail/${test_name}"
        COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++20 -fsyntax-only
            -I${NOTE_CPP_ROOT}/include
            ${src}
    )
    set_tests_properties("compile-fail/${test_name}" PROPERTIES WILL_FAIL TRUE)
endforeach()

# Compile-warn tests: verify that API misuse triggers a [[deprecated]] warning.
# Each .cpp file compiles clean without -Werror but fails with
# -Werror=deprecated-declarations (i.e. a [[deprecated]] attribute fired).
# The test passes if compilation fails.
file(GLOB COMPILE_WARN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_warn/*.cpp")
foreach(src ${COMPILE_WARN_SOURCES})
    get_filename_component(test_name ${src} NAME_WE)
    add_test(
        NAME "compile-warn/${test_name}"
        COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++20 -fsyntax-only
            -Werror=deprecated-declarations
            -I${NOTE_CPP_ROOT}/include
            ${src}
    )
    set_tests_properties("compile-warn/${test_name}" PROPERTIES WILL_FAIL TRUE)
endforeach()

# Compile-check tests: verify that expected includes/features work.
# Each .cpp file should compile successfully. The test passes if compilation succeeds.
# `-Werror=deprecated-declarations` catches regressions where a supported
# target/firmware is wrongly flagged as deprecated.
file(GLOB COMPILE_CHECK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_check/*.cpp")
foreach(src ${COMPILE_CHECK_SOURCES})
    get_filename_component(test_name ${src} NAME_WE)
    add_test(
        NAME "compile-check/${test_name}"
        COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++20 -fsyntax-only
            -Werror=deprecated-declarations
            -I${NOTE_CPP_ROOT}/include
            ${src}
    )
    # No WILL_FAIL — these must succeed
endforeach()

# Arduino build (same sources, ARDUINO defined globally like a real Arduino toolchain).
# Force-include the stubs header so Print/Printable are defined before any note-cpp header.
add_executable(note-cpp-tests-arduino
    ${NOTE_CPP_ROOT}/tests/doctest_main.cpp
    ${NOTE_CPP_ROOT}/tests/common/alloc_counter.cpp
    ${ALL_TEST_SOURCES}
)
target_compile_definitions(note-cpp-tests-arduino PRIVATE ARDUINO=10812 NOTE_ARDUINO_STUBS=1)
target_compile_options(note-cpp-tests-arduino PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/arduino_stubs.hpp)
add_test(NAME unit-tests-arduino COMMAND note-cpp-tests-arduino)

# ---------------------------------------------------------------------------
# Backend parity integration tests (doctest).
# Compiled on host here; compiled on device via
# tests/integration/firmware/platformio.ini's test_src_filter.
# Flag values match the existing tests/integration/*/CMakeLists.txt.
# ---------------------------------------------------------------------------

include(FetchContent)
FetchContent_Declare(
    cJSON
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.18
)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cJSON)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# Backend-parity tests need the polymorphic note::Notecard class, which
# NOTE_MINIMAL strips. Skip the target on minimal builds.
if(_minimal_pos EQUAL -1 AND _no_poly_pos EQUAL -1)
    add_executable(note-cpp-integration-backends
        ${NOTE_CPP_ROOT}/tests/doctest_main.cpp
        ${NOTE_CPP_ROOT}/tests/common/alloc_counter.cpp
        ${NOTE_CPP_ROOT}/tests/integration/cjson/test_cjson_backend.cpp
        ${NOTE_CPP_ROOT}/tests/integration/nlohmann/test_nlohmann_backend.cpp
        ${NOTE_CPP_ROOT}/tests/integration/buffer/test_buffer_backend.cpp
        ${NOTE_CPP_ROOT}/tests/integration/buffer/test_alloc_profile.cpp
        ${NOTE_CPP_ROOT}/tests/integration/buffer/test_sax_alloc_profile.cpp
        ${NOTE_CPP_ROOT}/tests/integration/buffer/test_sax_parser.cpp
        ${NOTE_CPP_ROOT}/tests/integration/cjson/test_alloc_profile.cpp
    )
    target_include_directories(note-cpp-integration-backends PRIVATE
        ${NOTE_CPP_ROOT}/include
        ${NOTE_CPP_ROOT}/tests
        ${cjson_SOURCE_DIR}
    )
    target_link_libraries(note-cpp-integration-backends PRIVATE cjson nlohmann_json::nlohmann_json)
    add_test(NAME integration-backends COMMAND note-cpp-integration-backends)
endif()
