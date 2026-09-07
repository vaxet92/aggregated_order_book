cmake_minimum_required(VERSION 3.20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg integration (only if not already set)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(consolidated_book CXX)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build latency micro-benchmarks" OFF)

# Sanitizers. address and thread are MUTUALLY EXCLUSIVE - clang rejects
# -fsanitize=address,thread - so they get separate build directories
# (see CMakePresets.json). UBSan is compatible with ASan and rides along
# at no extra cost.
#
# Set here, before any add_subdirectory, so the flags reach every target.
# Compile AND link flags are both required: a sanitizer that is compiled in
# but not linked fails at link time, and one that is linked but not compiled
# silently detects nothing.
#
# NOTE: LeakSanitizer is unsupported on macOS arm64, so the address build
# finds memory ERRORS but not leaks.
#
# NOTE: TSan cannot see inside uninstrumented vcpkg libraries (boost, gRPC,
# OpenSSL), so it may report false positives from their internals. Our own
# code is the signal; unit_tests is where that signal is cleanest.
set(SANITIZER "none" CACHE STRING "Sanitizer: none | address | thread")
set_property(CACHE SANITIZER PROPERTY STRINGS none address thread)

if(SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
elseif(SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
elseif(NOT SANITIZER STREQUAL "none")
    message(FATAL_ERROR "SANITIZER must be none, address, or thread (got '${SANITIZER}')")
endif()

add_subdirectory(types)
add_subdirectory(config)
add_subdirectory(logger)
add_subdirectory(md_core)
add_subdirectory(md_provider)
add_subdirectory(md_wire)
add_subdirectory(md_proto)
add_subdirectory(aggregator)
add_subdirectory(client)


if (BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/unit_tests)
  add_subdirectory(tests/mock_tests)
endif()

if (BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
