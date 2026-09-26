cmake_minimum_required(VERSION 3.25)

project(
    tazer
    VERSION 0.1.0
    DESCRIPTION "TAZER reference daemon"
    LANGUAGES C)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(TAZER_BUILD_TESTS
       "Build the daemon unit tests (requires a C++ compiler)" ON)
option(TAZER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
# Selected by the linux-asan / linux-tsan presets. Instruments every target
# built by this project (daemon, generated protos, nanopb runtime, tests);
# Conan-built dependencies are not instrumented.
set(TAZER_SANITIZER
    "none"
    CACHE STRING "Sanitizer: none, address (ASan+UBSan) or thread (TSan)")
set_property(CACHE TAZER_SANITIZER PROPERTY STRINGS none address thread)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static MSVC runtime on Windows (/MT, /MTd). No-op elsewhere.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Build identifier (short git hash, "unknown" outside a checkout)
# ---------------------------------------------------------------------------
find_package(Git QUIET)
set(TAZER_BUILD_ID "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _tazer_git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_tazer_git_hash)
        set(TAZER_BUILD_ID "${_tazer_git_hash}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Warning flags shared by all first-party targets
# ---------------------------------------------------------------------------
add_library(tazer_warnings INTERFACE)
if(MSVC)
    target_compile_options(tazer_warnings INTERFACE /W4 /permissive-)
    if(TAZER_WARNINGS_AS_ERRORS)
        target_compile_options(tazer_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(tazer_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(
        tazer_warnings
        INTERFACE -Wall
                  -Wextra
                  -Wpedantic
                  -Wshadow
                  -Wconversion
                  -Wsign-conversion
                  -Wstrict-prototypes
                  -Wmissing-prototypes)
    if(TAZER_WARNINGS_AS_ERRORS)
        target_compile_options(tazer_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Sanitizers (GCC/Clang only; MSVC's /fsanitize=address is a later addition)
# ---------------------------------------------------------------------------
if(NOT TAZER_SANITIZER STREQUAL "none")
    if(MSVC)
        message(FATAL_ERROR "TAZER_SANITIZER is not supported with MSVC yet")
    endif()
    if(TAZER_SANITIZER STREQUAL "address")
        set(_tazer_san_flags -fsanitize=address,undefined
                             -fno-sanitize-recover=undefined)
    elseif(TAZER_SANITIZER STREQUAL "thread")
        set(_tazer_san_flags -fsanitize=thread)
    else()
        message(FATAL_ERROR "Unknown TAZER_SANITIZER '${TAZER_SANITIZER}'")
    endif()
    add_compile_options(${_tazer_san_flags} -fno-omit-frame-pointer -g)
    add_link_options(${_tazer_san_flags})
    message(STATUS "TAZER: building with -fsanitize=${TAZER_SANITIZER}")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# The daemon itself is pure C. GoogleTest is C++, so a C++ compiler is only
# enabled when tests are built; doing it here (before the Conan provider runs)
# lets Conan build gtest with the same compiler CMake selected.
if(TAZER_BUILD_TESTS)
    enable_language(CXX)
endif()

find_package(libuv REQUIRED CONFIG)

# nanopb runtime, vendored as a git submodule pinned to the same release as the
# generator used by `just proto`.
set(TAZER_NANOPB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanopb)
if(NOT EXISTS ${TAZER_NANOPB_DIR}/pb.h)
    message(FATAL_ERROR "nanopb submodule missing at ${TAZER_NANOPB_DIR}. "
                        "Run: git submodule update --init --recursive")
endif()
add_library(
    nanopb STATIC
    ${TAZER_NANOPB_DIR}/pb_common.c ${TAZER_NANOPB_DIR}/pb_decode.c
    ${TAZER_NANOPB_DIR}/pb_encode.c)
target_include_directories(nanopb SYSTEM PUBLIC ${TAZER_NANOPB_DIR})
add_library(nanopb::nanopb ALIAS nanopb)

# ---------------------------------------------------------------------------
# Generated protocol messages (checked in; regenerate with `just proto`)
# ---------------------------------------------------------------------------
set(TAZER_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)
file(GLOB TAZER_PROTO_SOURCES CONFIGURE_DEPENDS
     ${TAZER_GENERATED_DIR}/tazer/v1/*.pb.c)
if(NOT TAZER_PROTO_SOURCES)
    message(
        FATAL_ERROR "No generated protocol sources in ${TAZER_GENERATED_DIR}. "
                    "Run: just proto")
endif()
add_library(tazer_proto STATIC ${TAZER_PROTO_SOURCES})
target_include_directories(tazer_proto SYSTEM PUBLIC ${TAZER_GENERATED_DIR})
target_link_libraries(tazer_proto PUBLIC nanopb::nanopb)
add_library(tazer::proto ALIAS tazer_proto)

# ---------------------------------------------------------------------------
# Daemon executable
# ---------------------------------------------------------------------------
add_executable(tazer src/main.c)
target_include_directories(tazer PRIVATE include)
# Strict -std=c11 hides POSIX declarations that libuv's headers rely on
# (pthread_rwlock_t, pthread_barrier_t); libuv documents _GNU_SOURCE on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(tazer PRIVATE _GNU_SOURCE)
endif()
target_compile_definitions(tazer PRIVATE TAZER_VERSION="${PROJECT_VERSION}"
                                         TAZER_BUILD_ID="${TAZER_BUILD_ID}")
# Conan's libuv recipe exports the static library as libuv::uv_a.
target_link_libraries(tazer PRIVATE tazer_warnings tazer::proto libuv::uv_a)

install(TARGETS tazer)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(TAZER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
