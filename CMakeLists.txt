cmake_minimum_required(VERSION 3.21)
project(gpu_fabric_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# std::format and complete <ranges> require GCC 13 / Clang 17.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR "GCC 13+ required (found ${CMAKE_CXX_COMPILER_VERSION}). "
                      "On Ubuntu 22.04: add ppa:ubuntu-toolchain-r/test, install g++-13, "
                      "and configure with CXX=g++-13.")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
  message(FATAL_ERROR "Clang 17+ required (found ${CMAKE_CXX_COMPILER_VERSION}).")
endif()

option(FABRIC_WERROR    "Treat warnings as errors (on in CI)" OFF)
option(FABRIC_SANITIZE  "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(FABRIC_BUILD_TESTS "Build the test suite" ON)

# ---------------------------------------------------------------------------
# Shared compile settings.
#
# -Wconversion is deliberate, not incidental: this codebase is integer-only by
# design (see docs/design.md) and a silent narrowing in the time or byte
# arithmetic is exactly the class of bug that would quietly invalidate every
# published number.
# ---------------------------------------------------------------------------
add_library(fabric_flags INTERFACE)
target_compile_options(fabric_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
  -Wnull-dereference -Wdouble-promotion
)
if(FABRIC_WERROR)
  target_compile_options(fabric_flags INTERFACE -Werror)
endif()
if(FABRIC_SANITIZE)
  target_compile_options(fabric_flags INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(fabric_flags INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# fabric_core
# ---------------------------------------------------------------------------
add_library(fabric_core STATIC
  src/fabric/model/fabric.cpp
  src/fabric/topology/leaf_spine.cpp
  src/fabric/topology/fat_tree.cpp
  src/fabric/routing/route_table.cpp
  src/fabric/core/simulation.cpp
  src/fabric/workload/permutation.cpp
  src/fabric/metrics/fct.cpp
  src/fabric/harness/scenario.cpp
)
target_include_directories(fabric_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fabric_core PUBLIC fabric_flags)

# ---------------------------------------------------------------------------
# fabricsim CLI
# ---------------------------------------------------------------------------
add_executable(fabricsim apps/fabricsim/main.cpp)
target_link_libraries(fabricsim PRIVATE fabric_core)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(FABRIC_BUILD_TESTS)
  enable_testing()

  # doctest is vendored (third_party/doctest.h), not fetched, so the build works
  # with no network access. SYSTEM keeps its warnings out of our -Werror builds.
  add_library(doctest INTERFACE)
  target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

  add_executable(fabric_tests
    tests/main.cpp
    tests/unit/test_event_queue.cpp
    tests/unit/test_units.cpp
    tests/unit/test_topology.cpp
    tests/unit/test_fat_tree.cpp
    tests/unit/test_routing.cpp
    tests/unit/test_ecmp_hash.cpp
    tests/unit/test_metrics.cpp
    tests/integration/test_analytical_fct.cpp
    tests/integration/test_determinism.cpp
    tests/integration/test_permutation.cpp
  )
  target_link_libraries(fabric_tests PRIVATE fabric_core doctest)

  add_test(NAME fabric_tests COMMAND fabric_tests --reporters=console)
endif()
