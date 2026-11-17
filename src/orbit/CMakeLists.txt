find_package(nlohmann_json 3.11 REQUIRED)

satsim_add_library(orbit SOURCES omm_loader.cpp)
# Both are implementation details: the public headers use neither.
target_link_libraries(satsim_orbit PRIVATE satsim::core nlohmann_json::nlohmann_json)
