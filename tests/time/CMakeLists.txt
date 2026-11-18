satsim_add_test(satsim_time_tests
  SOURCES civil_time_test.cpp utc_time_test.cpp gmst_test.cpp time_scales_test.cpp earth_orientation_test.cpp
  DEPS satsim::time)
