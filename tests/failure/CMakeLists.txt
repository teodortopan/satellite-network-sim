# The failure module, on small shells (fixture.cpp). The topology tests' ideal.cpp classifies
# ideal Walker shells by construction; it is not a library, so it is compiled in here too.
satsim_add_test(satsim_failure_tests
  SOURCES ../topology/ideal.cpp fixture.cpp schedule_test.cpp failure_file_test.cpp
    detection_test.cpp
  DEPS satsim::failure satsim::topology satsim::test_support satsim::constellation
    satsim::ground satsim::orbit satsim::frames satsim::time satsim::core)
