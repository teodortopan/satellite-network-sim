# The failures program's core (apps/failures), called directly: the timeline and views on a
# hand-made geometry (fake_geometry.cpp) over the failure tests' small network
# (tests/failure/fixture.cpp, world.cpp), re-matching on ideal shells (tests/topology/ideal.cpp,
# not a library, so compiled in), and the real geometry against app::StepSource on the topology
# program's synthetic snapshot and ground fixture (tests/topology_app/fixture.cpp). All small
# enough for the sanitizer and Valgrind runs.
satsim_add_test(satsim_failures_app_tests
  SOURCES ../topology/ideal.cpp ../failure/fixture.cpp ../topology_app/fixture.cpp
    fake_geometry.cpp world.cpp settings_test.cpp control_graph_test.cpp timeline_test.cpp
    view_test.cpp repoint_test.cpp snapshot_geometry_test.cpp loop_test.cpp flooding_test.cpp
    arguments_test.cpp run_test.cpp
  DEPS satsim::failures_app satsim::topology_app satsim::test_support satsim::routing_app
    satsim::app_inputs satsim::failure satsim::network satsim::topology satsim::constellation
    satsim::ground satsim::orbit satsim::frames satsim::time satsim::core)
target_include_directories(satsim_failures_app_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate")

# The committed snapshot and ground data, against the routing program (spec 6.2), and one run
# with a failure. Kept out of the sanitizer builds and out of Valgrind like the other snapshot
# checks; its name does not end in _tests.
if(NOT SATSIM_ENABLE_SANITIZERS)
  add_executable(satsim_failures_app_real_data_check real_data_check.cpp)
  target_link_libraries(satsim_failures_app_real_data_check
    PRIVATE satsim::failures_app satsim::routing_app GTest::gtest_main)
  target_include_directories(satsim_failures_app_real_data_check
    PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate")
  satsim_set_warnings(satsim_failures_app_real_data_check)
  gtest_discover_tests(satsim_failures_app_real_data_check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES LABELS snapshot)
endif()
