find_package(Eigen3 3.4 REQUIRED NO_MODULE)

satsim_add_library(frames SOURCES teme_ecef.cpp geodetic.cpp DEPS satsim::time Eigen3::Eigen)
