# The topology module. ideal.cpp classifies ideal Walker shells by construction (fast enough
# for the sanitizer and Valgrind runs); the shells come from tests/support.
satsim_add_test(satsim_topology_tests
  SOURCES ideal.cpp geometry_test.cpp link_plan_test.cpp link_state_test.cpp
    ground_links_test.cpp physical_checks_test.cpp
  DEPS satsim::topology satsim::test_support satsim::constellation satsim::ground
    satsim::orbit satsim::frames satsim::time satsim::core)

# The committed snapshot at the start of the Phase 2 run. Kept out of the sanitizer builds and
# out of Valgrind like the other snapshot checks; its name does not end in _tests.
if(NOT SATSIM_ENABLE_SANITIZERS)
  add_executable(satsim_topology_snapshot_check snapshot_test.cpp)
  target_link_libraries(satsim_topology_snapshot_check
    PRIVATE satsim::topology satsim::constellation satsim::ground satsim::orbit satsim::time
      GTest::gtest_main)
  satsim_set_warnings(satsim_topology_snapshot_check)
  gtest_discover_tests(satsim_topology_snapshot_check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES LABELS snapshot)
endif()
