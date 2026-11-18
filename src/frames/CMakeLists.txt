find_package(Eigen3 3.4 REQUIRED NO_MODULE)

satsim_add_library(frames
  SOURCES teme_ecef.cpp geodetic.cpp precession_nutation.cpp teme_j2000.cpp
  DEPS satsim::time Eigen3::Eigen)
