# The failures program (Phase 4). Its logic is a library, so tests call it directly: what a
# failure schedule causes over time, worked out before the run steps (timeline.hpp), the
# geometry it reads at any step (geometry.hpp), the control graph reports travel on
# (control_graph.hpp), the true network and each actor's view at an instant (view.hpp), the
# lanes stepped through the run (loop.hpp), the measures (measures.hpp), the reports
# (reports.hpp) and the program (arguments.hpp, run.hpp). The executable is only main().
satsim_add_library(failures_app
  SOURCES settings.cpp geometry.cpp control_graph.cpp timeline.cpp view.cpp loop.cpp
    measures.cpp reports.cpp arguments.cpp run.cpp
  DEPS satsim::routing_app satsim::app_inputs satsim::app_common satsim::failure satsim::network
    satsim::routing satsim::topology satsim::constellation satsim::ground satsim::orbit
    satsim::frames satsim::time)
# Headers are included as "failures/timeline.hpp".
target_include_directories(satsim_failures_app PUBLIC "${PROJECT_SOURCE_DIR}/apps")
target_link_libraries(satsim_failures_app PRIVATE satsim::core)

# Output in <build>/bin.
add_executable(failures main.cpp)
target_link_libraries(failures PRIVATE satsim::failures_app)
set_target_properties(failures PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
satsim_set_warnings(failures)
satsim_enable_sanitizers(failures)
