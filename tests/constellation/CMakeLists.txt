# The constellation module. synthetic.cpp builds ideal Walker shells; it reads SGP4's secular
# rates from Vallado's sgp4init (hence the vendored library and src/orbit's internal header) to
# move an element set to another epoch exactly.
satsim_add_test(satsim_constellation_tests
  SOURCES geometry_test.cpp classification_test.cpp synthetic.cpp
  DEPS satsim::constellation satsim::orbit satsim::sgp4 satsim::time satsim::core)
target_include_directories(satsim_constellation_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/orbit")

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
