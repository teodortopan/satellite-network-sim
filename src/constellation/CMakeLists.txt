find_package(Eigen3 3.4 REQUIRED NO_MODULE)
find_package(nlohmann_json 3.11 REQUIRED)

satsim_add_library(constellation
  SOURCES geometry.cpp classification.cpp config_file.cpp
  DEPS satsim::orbit satsim::time Eigen3::Eigen)
target_link_libraries(satsim_constellation PRIVATE satsim::core satsim::frames
  nlohmann_json::nlohmann_json)
