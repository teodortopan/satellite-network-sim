cmake_minimum_required(VERSION 3.25)

project(satsim
  VERSION 0.1.0
  DESCRIPTION "Satellite constellation network simulator"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SATSIM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in satsim targets" OFF)
option(SATSIM_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SATSIM_BUILD_TESTS "Build the satsim unit tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)

include(SatsimTargets)

# Vendored third-party code, built unmodified (see third_party/sgp4/README.md).
satsim_add_third_party_library(sgp4
  SOURCES third_party/sgp4/SGP4.cpp
  INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sgp4")

# One library per module. Later modules may depend on earlier ones.
add_subdirectory(src/core)
add_subdirectory(src/time)
add_subdirectory(src/frames)
add_subdirectory(src/ground)
add_subdirectory(src/orbit)
add_subdirectory(src/constellation)

# Programs.
add_subdirectory(apps/propagate)
add_subdirectory(apps/constellation)

if(SATSIM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
