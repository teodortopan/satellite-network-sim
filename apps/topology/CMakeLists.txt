# The topology program. Its logic is a library, so tests call it directly and it gets our
# warnings and sanitizers; the executable is only main(). The two endpoint models
# (endpoints.hpp) live here, on top of the generic shortest path (satsim::network).
satsim_add_library(topology_app
  SOURCES arguments.cpp endpoints.cpp run.cpp
  DEPS satsim::app_common satsim::topology satsim::network satsim::constellation
    satsim::ground satsim::orbit satsim::time)
# Headers are included as "topology/run.hpp".
target_include_directories(satsim_topology_app PUBLIC "${PROJECT_SOURCE_DIR}/apps")
target_link_libraries(satsim_topology_app PRIVATE satsim::core)

# Output in <build>/bin.
add_executable(topology main.cpp)
target_link_libraries(topology PRIVATE satsim::topology_app)
set_target_properties(topology PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
satsim_set_warnings(topology)
satsim_enable_sanitizers(topology)
