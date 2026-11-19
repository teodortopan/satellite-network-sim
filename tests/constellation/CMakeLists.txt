# The constellation module. Ideal Walker shells come from tests/support (satsim::test_support).
satsim_add_test(satsim_constellation_tests
  SOURCES geometry_test.cpp classification_test.cpp config_file_test.cpp
  DEPS satsim::constellation satsim::test_support satsim::orbit satsim::time satsim::core)

# Classifies the full committed snapshot. Kept out of the sanitizer builds and out of Valgrind,
# like tests/orbit's snapshot check, for the same reason (parsing the snapshot under GCC's ASan
# takes minutes); its name does not end in _tests.
if(NOT SATSIM_ENABLE_SANITIZERS)
  add_executable(satsim_constellation_snapshot_check snapshot_test.cpp)
  target_link_libraries(satsim_constellation_snapshot_check
    PRIVATE satsim::constellation satsim::orbit GTest::gtest_main)
  satsim_set_warnings(satsim_constellation_snapshot_check)
  gtest_discover_tests(satsim_constellation_snapshot_check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES LABELS snapshot)
endif()
