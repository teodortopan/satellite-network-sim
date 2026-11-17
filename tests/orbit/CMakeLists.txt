find_package(nlohmann_json 3.11 REQUIRED)

satsim_add_test(satsim_orbit_tests SOURCES omm_loader_test.cpp DEPS satsim::orbit)

# Loads the full committed snapshot (about 22 000 records). Kept out of the sanitizer builds
# and out of the Valgrind run: under GCC's ASan (stack-use-after-return detection, on by
# default) nlohmann parsing is about 800x slower, about 5 minutes per file. It runs natively
# in the optimised build of the valgrind step's ctest pass instead; its name deliberately does
# not end in _tests, which is what scripts/ci.sh runs under Valgrind.
if(NOT SATSIM_ENABLE_SANITIZERS)
  add_executable(satsim_orbit_snapshot_check snapshot_test.cpp)
  target_link_libraries(satsim_orbit_snapshot_check
    PRIVATE satsim::orbit nlohmann_json::nlohmann_json GTest::gtest_main)
  satsim_set_warnings(satsim_orbit_snapshot_check)
  gtest_discover_tests(satsim_orbit_snapshot_check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES LABELS snapshot)
endif()
