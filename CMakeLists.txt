cmake_minimum_required(VERSION 3.16)
project(limit_order_book VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS    "Build GoogleTest unit tests" ON)
option(LOB_BUILD_BENCH    "Build the throughput benchmark" ON)
option(LOB_BUILD_EXAMPLES "Build the demo" ON)
option(LOB_SANITIZE       "Enable AddressSanitizer + UBSan" OFF)

# Header-only library -------------------------------------------------------
add_library(lob INTERFACE)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(lob INTERFACE cxx_std_17)

# Shared warning / sanitizer flags -----------------------------------------
add_library(lob_flags INTERFACE)
target_compile_options(lob_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
if(LOB_SANITIZE)
  target_compile_options(lob_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_flags INTERFACE -fsanitize=address,undefined)
endif()

# Unit tests ----------------------------------------------------------------
if(LOB_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "System GoogleTest not found - fetching v1.14.0")
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(lob_tests tests/test_order_book.cpp)
  target_link_libraries(lob_tests PRIVATE lob lob_flags GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()

# Benchmark (frame pointers + symbols kept so `perf` can unwind stacks) ------
if(LOB_BUILD_BENCH)
  add_executable(bench_book bench/bench_book.cpp)
  target_include_directories(bench_book PRIVATE bench)
  target_link_libraries(bench_book PRIVATE lob lob_flags)
  target_compile_options(bench_book PRIVATE -O3 -g -fno-omit-frame-pointer)
endif()

if(LOB_BUILD_EXAMPLES)
  add_executable(demo examples/demo.cpp)
  target_link_libraries(demo PRIVATE lob lob_flags)
endif()
