# Helpers that give every satsim library and test the same settings: C++20, our
# warning set, and sanitizers when enabled.

# satsim_add_library(<name> SOURCES <files...> [DEPS <targets...>])
# Creates static library satsim_<name> with alias satsim::<name>. DEPS are linked PUBLIC.
function(satsim_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  set(target satsim_${name})
  add_library(${target} STATIC ${ARG_SOURCES})
  add_library(satsim::${name} ALIAS ${target})
  target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_link_libraries(${target} PUBLIC ${ARG_DEPS})
  satsim_set_warnings(${target})
  satsim_enable_sanitizers(${target})
endfunction()

# satsim_add_third_party_library(<name> SOURCES <files...> INCLUDE_DIR <dir>)
# Creates static library satsim_<name> with alias satsim::<name> from vendored, unmodified
# third-party sources. It gets the sanitizers (so the whole test binary is instrumented
# consistently) but not our warning flags, the same treatment GoogleTest gets. INCLUDE_DIR is
# a SYSTEM include directory, so the compiler and clang-tidy do not report warnings from its
# headers inside our targets. scripts/ci.sh never formats or tidies third_party/.
function(satsim_add_third_party_library name)
  cmake_parse_arguments(ARG "" "INCLUDE_DIR" "SOURCES" ${ARGN})
  set(target satsim_${name})
  add_library(${target} STATIC ${ARG_SOURCES})
  add_library(satsim::${name} ALIAS ${target})
  target_include_directories(${target} SYSTEM PUBLIC "${ARG_INCLUDE_DIR}")
  target_compile_features(${target} PUBLIC cxx_std_20)
  satsim_enable_sanitizers(${target})
endfunction()

# satsim_add_test(<name> SOURCES <files...> [DEPS <targets...>])
# Creates GoogleTest executable <name> (must end in _tests) in <build>/tests, where
# scripts/ci.sh looks for executables to run under Valgrind, and registers its cases
# with CTest. The working directory of every test is the repository root.
function(satsim_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_DEPS} GTest::gtest_main)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
  satsim_set_warnings(${name})
  satsim_enable_sanitizers(${name})
  gtest_discover_tests(${name} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endfunction()
