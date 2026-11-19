find_package(nlohmann_json 3.11 REQUIRED)

satsim_add_test(satsim_orbit_tests
  SOURCES omm_loader_test.cpp element_selection_test.cpp propagator_test.cpp
    physical_sanity_test.cpp snapshot_loader_test.cpp
  DEPS satsim::orbit satsim::core)
# propagator_test.cpp checks the internal ElementSet-to-SGP4 conversion (src/orbit).
target_include_directories(satsim_orbit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/orbit")

# Vallado's verification cases, run through his own code (which is why this links the vendored
# library directly) and through our Propagator.
satsim_add_test(satsim_sgp4_verification_tests
  SOURCES sgp4_verification_test.cpp sgp4_verification_data.cpp
  DEPS satsim::orbit satsim::sgp4 satsim::core satsim::time)
target_include_directories(satsim_sgp4_verification_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/orbit")

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
