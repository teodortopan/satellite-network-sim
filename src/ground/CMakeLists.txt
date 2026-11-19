find_package(nlohmann_json 3.11 REQUIRED)
find_package(Eigen3 3.4 REQUIRED NO_MODULE)

satsim_add_library(ground
  SOURCES address_block.cpp sites.cpp distance.cpp json_loaders.cpp csv_loaders.cpp
    home_pop.cpp
  DEPS satsim::frames Eigen3::Eigen)
# Implementation details: the public headers use none of these.
target_link_libraries(satsim_ground PRIVATE satsim::core nlohmann_json::nlohmann_json)
