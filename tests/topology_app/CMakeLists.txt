# The topology program's library (apps/topology), called directly. The synthetic snapshot
# comes from tests/support (satsim::test_support); the propagate tests' helpers (scratch
# directories, CSV reader) are shared.
satsim_add_test(satsim_topology_app_tests
  SOURCES fixture.cpp arguments_test.cpp endpoints_test.cpp run_test.cpp
  DEPS satsim::topology_app satsim::test_support satsim::topology satsim::network
    satsim::constellation satsim::ground satsim::orbit satsim::time satsim::core)
target_include_directories(satsim_topology_app_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate")

# The built executable, run as a child process (traced under the valgrind step).
satsim_add_test(satsim_topology_e2e_tests
  SOURCES fixture.cpp e2e_test.cpp
  DEPS satsim::topology_app satsim::test_support)
target_include_directories(satsim_topology_e2e_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate")
target_compile_definitions(satsim_topology_e2e_tests
  PRIVATE SATSIM_TOPOLOGY_EXE="$<TARGET_FILE:topology>")
add_dependencies(satsim_topology_e2e_tests topology)

# The committed snapshot and ground data, 5 minutes per pattern and model. Kept out of the
# sanitizer builds and out of Valgrind like the other snapshot checks; its name does not end
# in _tests.
if(NOT SATSIM_ENABLE_SANITIZERS)
  add_executable(satsim_topology_app_real_data_check real_data_check.cpp)
  target_link_libraries(satsim_topology_app_real_data_check
    PRIVATE satsim::topology_app GTest::gtest_main)
  target_include_directories(satsim_topology_app_real_data_check
    PRIVATE "${PROJECT_SOURCE_DIR}/tests/propagate")
  satsim_set_warnings(satsim_topology_app_real_data_check)
  gtest_discover_tests(satsim_topology_app_real_data_check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES LABELS snapshot)
endif()
