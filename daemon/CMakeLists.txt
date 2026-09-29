cmake_minimum_required(VERSION 3.25)

project(
    taz
    VERSION 0.1.0
    DESCRIPTION "TAZ reference daemon"
    LANGUAGES C)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(TAZ_BUILD_TESTS "Build the daemon unit tests (requires a C++ compiler)"
       ON)
option(TAZ_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
# Selected by the linux-asan / linux-tsan / windows-asan presets. Instruments
# every target built by this project (daemon, generated protos, nanopb runtime,
# tests); Conan-built dependencies are not instrumented. "address" means
# ASan+UBSan with GCC/Clang and ASan alone with MSVC (which has no UBSan or
# TSan).
set(TAZ_SANITIZER
    "none"
    CACHE STRING "Sanitizer: none, address (ASan[+UBSan]) or thread (TSan)")
set_property(CACHE TAZ_SANITIZER PROPERTY STRINGS none address thread)
# Selected by the linux-analyze / windows-analyze presets: the compiler's own
# static analyzer (GCC -fanalyzer, MSVC /analyze) on first-party code, with
# findings promoted to errors by TAZ_WARNINGS_AS_ERRORS.
option(TAZ_STATIC_ANALYSIS "Enable the compiler's static analyzer" OFF)
# Selected by the linux-coverage preset (GCC/Clang gcov instrumentation).
option(TAZ_COVERAGE "Instrument first-party code for coverage" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The daemon itself is pure C. GoogleTest is C++, so C++ is enabled only when
# tests are built. The C++ standard here must agree with compiler.cppstd in
# the Conan profiles (daemon/profiles/*), which is what gtest is built with.
if(TAZ_BUILD_TESTS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# Static MSVC runtime on Windows (/MT, /MTd). No-op elsewhere.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Build identifier (short git hash, "unknown" outside a checkout)
# ---------------------------------------------------------------------------
find_package(Git QUIET)
set(TAZ_BUILD_ID "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _taz_git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_taz_git_hash)
        set(TAZ_BUILD_ID "${_taz_git_hash}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Warning flags shared by all first-party targets
# ---------------------------------------------------------------------------
add_library(taz_warnings INTERFACE)
if(MSVC)
    target_compile_options(taz_warnings INTERFACE /W4 /permissive-)
    if(TAZ_WARNINGS_AS_ERRORS)
        target_compile_options(taz_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(taz_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(
        taz_warnings
        INTERFACE -Wall
                  -Wextra
                  -Wpedantic
                  -Wshadow
                  -Wconversion
                  -Wsign-conversion
                  -Wstrict-prototypes
                  -Wmissing-prototypes)
    if(TAZ_WARNINGS_AS_ERRORS)
        target_compile_options(taz_warnings INTERFACE -Werror)
    endif()
endif()

# C++ test targets get the same base warnings but without -Wstrict-prototypes
# and -Wmissing-prototypes (C-only flags invalid in C++ TUs). No analyzer or
# coverage flags here so linux-analyze and linux-coverage do not instrument
# gtest code.
add_library(taz_cxx_warnings INTERFACE)
if(MSVC)
    target_compile_options(taz_cxx_warnings INTERFACE /W4 /permissive-)
    if(TAZ_WARNINGS_AS_ERRORS)
        target_compile_options(taz_cxx_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(
        taz_cxx_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow
                                   -Wconversion -Wsign-conversion)
    if(TAZ_WARNINGS_AS_ERRORS)
        target_compile_options(taz_cxx_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler static analysis and coverage (first-party targets only, via the
# taz_warnings interface target)
# ---------------------------------------------------------------------------
if(TAZ_STATIC_ANALYSIS)
    if(MSVC)
        # /analyze:external- keeps SDK and dependency headers out of the report
        # (CMake passes SYSTEM includes as /external:I on MSVC >= 19.29).
        target_compile_options(taz_warnings INTERFACE /analyze
                                                      /analyze:external-)
        message(STATUS "TAZ: MSVC /analyze enabled")
    elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(taz_warnings INTERFACE -fanalyzer)
        message(STATUS "TAZ: GCC -fanalyzer enabled")
    else()
        message(
            WARNING
                "TAZ_STATIC_ANALYSIS: no analyzer flags for ${CMAKE_C_COMPILER_ID}; "
                "clang-tidy (just lint-c) covers the Clang analyzer")
    endif()
endif()

if(TAZ_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "TAZ_COVERAGE requires GCC or Clang (gcov)")
    endif()
    target_compile_options(taz_warnings INTERFACE --coverage -O0)
    target_link_options(taz_warnings INTERFACE --coverage)
    message(STATUS "TAZ: coverage instrumentation enabled")
endif()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
if(NOT TAZ_SANITIZER STREQUAL "none")
    if(MSVC)
        if(NOT TAZ_SANITIZER STREQUAL "address")
            message(
                FATAL_ERROR
                    "MSVC supports only TAZ_SANITIZER=address (no TSan/UBSan)")
        endif()
        # /fsanitize=address is incompatible with the /RTC1 runtime checks that
        # CMake puts in the default Debug flags, and with incremental linking.
        foreach(_lang C CXX)
            string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_${_lang}_FLAGS_DEBUG
                                 "${CMAKE_${_lang}_FLAGS_DEBUG}")
        endforeach()
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/INCREMENTAL:NO)
        # gtest is Conan-built without ASan; disable the STL container
        # annotations so std::string/std::vector crossing that boundary do not
        # raise false container-overflow reports (per Microsoft's guidance).
        add_compile_definitions(
            _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION
            _DISABLE_OPTIONAL_ANNOTATION) # optional: VS 2026 STL and later
        message(STATUS "TAZ: building with /fsanitize=address")
    else()
        if(TAZ_SANITIZER STREQUAL "address")
            set(_taz_san_flags -fsanitize=address,undefined
                               -fno-sanitize-recover=undefined)
        elseif(TAZ_SANITIZER STREQUAL "thread")
            set(_taz_san_flags -fsanitize=thread)
        else()
            message(FATAL_ERROR "Unknown TAZ_SANITIZER '${TAZ_SANITIZER}'")
        endif()
        add_compile_options(${_taz_san_flags} -fno-omit-frame-pointer -g)
        add_link_options(${_taz_san_flags})
        message(STATUS "TAZ: building with -fsanitize=${TAZ_SANITIZER}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Dependencies (Conan-generated config packages; `just configure` runs
# `conan install` before cmake and the preset's toolchain file adds the
# generators folder to CMAKE_PREFIX_PATH)
# ---------------------------------------------------------------------------
find_package(libuv REQUIRED CONFIG)
# libuv documents _GNU_SOURCE on Linux: strict -std=c11 hides POSIX
# declarations (pthread_rwlock_t, pthread_barrier_t) that libuv headers need.
# Propagate via libuv's INTERFACE so every target linking libuv::uv_a gets it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_property(
        TARGET libuv::uv_a
        APPEND
        PROPERTY INTERFACE_COMPILE_DEFINITIONS _GNU_SOURCE)
endif()

# nanopb runtime, vendored as a git submodule pinned to the same release as the
# generator used by `just proto`.
set(TAZ_NANOPB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanopb)
if(NOT EXISTS ${TAZ_NANOPB_DIR}/pb.h)
    message(FATAL_ERROR "nanopb submodule missing at ${TAZ_NANOPB_DIR}. "
                        "Run: git submodule update --init --recursive")
endif()
add_library(
    nanopb STATIC ${TAZ_NANOPB_DIR}/pb_common.c ${TAZ_NANOPB_DIR}/pb_decode.c
                  ${TAZ_NANOPB_DIR}/pb_encode.c)
target_include_directories(nanopb SYSTEM PUBLIC ${TAZ_NANOPB_DIR})
add_library(nanopb::nanopb ALIAS nanopb)

# ---------------------------------------------------------------------------
# Generated protocol messages (checked in; regenerate with `just proto`)
# ---------------------------------------------------------------------------
set(TAZ_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)
file(GLOB TAZ_PROTO_SOURCES CONFIGURE_DEPENDS
     ${TAZ_GENERATED_DIR}/taz/v1/*.pb.c)
if(NOT TAZ_PROTO_SOURCES)
    message(
        FATAL_ERROR "No generated protocol sources in ${TAZ_GENERATED_DIR}. "
                    "Run: just proto")
endif()
add_library(taz_proto STATIC ${TAZ_PROTO_SOURCES})
target_include_directories(taz_proto SYSTEM PUBLIC ${TAZ_GENERATED_DIR})
target_link_libraries(taz_proto PUBLIC nanopb::nanopb)
add_library(taz::proto ALIAS taz_proto)

# ---------------------------------------------------------------------------
# Build-info seam (generated from the git hash and PROJECT_VERSION at configure
# time; stale hashes require a reconfigure)
# ---------------------------------------------------------------------------
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/build_info.c.in
               ${CMAKE_CURRENT_BINARY_DIR}/build_info.c @ONLY)

# ---------------------------------------------------------------------------
# Core logic library (frame pack/unpack/validate; shared by daemon and tests)
# ---------------------------------------------------------------------------
add_library(
    taz_core STATIC
    src/frame.c
    src/reassembly.c
    src/payload.c
    src/error.c
    src/server.c
    src/dispatch.c
    src/connection.c
    src/handlers/version.c
    ${CMAKE_CURRENT_BINARY_DIR}/build_info.c)
target_include_directories(taz_core PUBLIC include)
# Stop <windows.h> from including the old <winsock.h>, which clashes with the
# <winsock2.h> that uv.h includes. clang-format sorts <windows.h> ahead of our
# own headers, so include order alone cannot prevent it.
if(WIN32)
    target_compile_definitions(taz_core PUBLIC WIN32_LEAN_AND_MEAN)
endif()
# taz_warnings is PRIVATE so C-only flags (-Wstrict-prototypes) do not leak
# to the C++ GoogleTest target that links taz_core.
target_link_libraries(
    taz_core
    PRIVATE taz_warnings
    PUBLIC taz::proto libuv::uv_a)

# ---------------------------------------------------------------------------
# Daemon executable
# ---------------------------------------------------------------------------
# Platform-specific signal handling and I/O flushing live in separate files so
# Win32 and POSIX code are never compiled together.
if(WIN32)
    set(TAZ_PLATFORM_SRC src/platform_win32.c)
else()
    set(TAZ_PLATFORM_SRC src/platform_posix.c)
endif()

add_executable(tazd src/main.c ${TAZ_PLATFORM_SRC})
# include/, _GNU_SOURCE, taz::proto, and libuv::uv_a come in transitively.
target_link_libraries(tazd PRIVATE taz_warnings taz_core)

install(TARGETS tazd)

# ---------------------------------------------------------------------------
# Fuzz harnesses (linux-fuzz preset only)
# ---------------------------------------------------------------------------
option(TAZ_BUILD_FUZZ
       "Build the libFuzzer harnesses (requires Clang + -fsanitize=fuzzer)" OFF)
if(TAZ_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(TAZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
