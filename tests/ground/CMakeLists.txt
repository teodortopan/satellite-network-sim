satsim_add_test(satsim_ground_tests
  SOURCES address_block_test.cpp distance_test.cpp sites_test.cpp loaders_test.cpp
    home_pop_test.cpp
  DEPS satsim::ground satsim::core)

# The committed ground data (data/ground/). Kept out of the sanitizer builds and out of
# Valgrind, like the snapshot checks: under GCC's ASan, parsing the 13,103-line geofeed and
# the PoP file takes about 77 s, and CTest runs every case in its own process, so each case
# paid it again. It runs natively in the optimised build of the valgrind step's ctest pass;
# its name does not end in _tests.
if(NOT SATSIM_ENABLE_SANITIZERS)
  add_executable(satsim_ground_real_data_check real_data_test.cpp)
  target_link_libraries(satsim_ground_real_data_check
    PRIVATE satsim::ground satsim::core GTest::gtest_main)
  satsim_set_warnings(satsim_ground_real_data_check)
  gtest_discover_tests(satsim_ground_real_data_check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES LABELS snapshot)
endif()
