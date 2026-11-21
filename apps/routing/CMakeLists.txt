# The routing program (Phase 3). Its logic is a library, so tests call it directly and it gets
# our warnings and sanitizers; the executable is only main(). The graph of spec 4.2
# (graph.hpp) and the planner loop (engine.hpp) are pure and testable without a snapshot; the
# program (run.hpp) feeds them the propagated steps.
satsim_add_library(routing_app
  SOURCES settings.cpp graph.cpp engine.cpp arguments.cpp run.cpp
  DEPS satsim::app_inputs satsim::app_common satsim::routing satsim::network satsim::topology
    satsim::constellation satsim::ground satsim::orbit satsim::time)
# Headers are included as "routing/run.hpp".
target_include_directories(satsim_routing_app PUBLIC "${PROJECT_SOURCE_DIR}/apps")
target_link_libraries(satsim_routing_app PRIVATE satsim::core)

# Output in <build>/bin.
add_executable(routing main.cpp)
target_link_libraries(routing PRIVATE satsim::routing_app)
set_target_properties(routing PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
satsim_set_warnings(routing)
satsim_enable_sanitizers(routing)
