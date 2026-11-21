# The routing program's library (apps/routing), called directly. The graph and engine tests
# run on hand-made worlds (fake.cpp); the program tests on the topology program's synthetic
# snapshot and ground fixture (tests/topology_app/fixture.cpp), against the topology program
# itself for spec 6.2. The propagate tests' helpers (scratch directories, CSV reader) are
# shared.
satsim_add_test(satsim_routing_app_tests
  SOURCES fake.cpp ../topology_app/fixture.cpp graph_test.cpp engine_test.cpp arguments_test.cpp
    run_test.cpp
  DEPS satsim::routing_app satsim::topology_app satsim::test_support satsim::app_inputs
    satsim::routing satsim::network satsim::topology satsim::constellation satsim::ground
    satsim::orbit satsim::time satsim::core)
target_include_directories(satsim_routing_app_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate" "${PROJECT_SOURCE_DIR}/tests/topology_app")

# The committed snapshot and ground data, against the topology program (spec 6.2). Kept out of
# the sanitizer builds and out of Valgrind like the other snapshot checks; its name does not
# end in _tests.
if(NOT SATSIM_ENABLE_SANITIZERS)
  add_executable(satsim_routing_app_real_data_check real_data_check.cpp)
  target_link_libraries(satsim_routing_app_real_data_check
    PRIVATE satsim::routing_app satsim::topology_app GTest::gtest_main)
  target_include_directories(satsim_routing_app_real_data_check
    PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate")
  satsim_set_warnings(satsim_routing_app_real_data_check)
  gtest_discover_tests(satsim_routing_app_real_data_check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES LABELS snapshot)
endif()
