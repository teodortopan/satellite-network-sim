find_package(Eigen3 3.4 REQUIRED NO_MODULE)

satsim_add_library(constellation
  SOURCES geometry.cpp classification.cpp
  DEPS satsim::orbit satsim::time Eigen3::Eigen)
target_link_libraries(satsim_constellation PRIVATE satsim::core satsim::frames)
