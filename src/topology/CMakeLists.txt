find_package(Eigen3 3.4 REQUIRED NO_MODULE)

satsim_add_library(topology
  SOURCES positions.cpp geometry.cpp link_plan.cpp link_state.cpp ground_links.cpp
    physical_checks.cpp link_file.cpp
  DEPS satsim::constellation satsim::ground satsim::orbit satsim::frames satsim::time
    Eigen3::Eigen)
target_link_libraries(satsim_topology PRIVATE satsim::core)
