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

add_library(satsim_core
  src/core/angles.cpp)
add_library(satsim::core ALIAS satsim_core)
target_include_directories(satsim_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(satsim_core PUBLIC cxx_std_20)
satsim_set_warnings(satsim_core)
satsim_enable_sanitizers(satsim_core)

if(SATSIM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
