find_package(nlohmann_json 3.11 REQUIRED)
find_package(Eigen3 3.4 REQUIRED NO_MODULE)

satsim_add_library(orbit
  SOURCES omm_loader.cpp element_selection.cpp sgp4_inputs.cpp propagator.cpp snapshot.cpp
  DEPS satsim::time Eigen3::Eigen)
# Implementation details: the public headers use none of these. In particular no Vallado type
# or header appears in include/satsim.
target_link_libraries(satsim_orbit PRIVATE satsim::core satsim::sgp4 nlohmann_json::nlohmann_json)
