# The constellation program's library (apps/constellation), called directly. The propagate
# tests' helpers (fixture snapshot, scratch directories, CSV reader) are shared.
satsim_add_test(satsim_constellation_app_tests
  SOURCES arguments_test.cpp run_test.cpp
  DEPS satsim::constellation_app satsim::constellation satsim::propagate_app satsim::time
    satsim::core)
target_include_directories(satsim_constellation_app_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate")

# The built executable, run as a child process (traced under the valgrind step).
satsim_add_test(satsim_constellation_e2e_tests
  SOURCES e2e_test.cpp
  DEPS satsim::propagate_app)
target_include_directories(satsim_constellation_e2e_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate")
target_compile_definitions(satsim_constellation_e2e_tests
  PRIVATE SATSIM_CONSTELLATION_EXE="$<TARGET_FILE:constellation>")
add_dependencies(satsim_constellation_e2e_tests constellation)
