satsim_add_test(satsim_frames_tests
  SOURCES wgs84_test.cpp teme_ecef_test.cpp geodetic_test.cpp teme_j2000_test.cpp
  DEPS satsim::frames)
