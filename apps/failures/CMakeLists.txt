# The failures program's core (Phase 4): what a failure schedule causes over time, worked out
# before the run steps (timeline.hpp), the geometry it reads at any step (geometry.hpp), the
# control graph reports travel on (control_graph.hpp), and the true network and each actor's
# view at an instant (view.hpp). A library, so tests call it directly. No executable yet.
satsim_add_library(failures_app
  SOURCES settings.cpp geometry.cpp control_graph.cpp timeline.cpp view.cpp
  DEPS satsim::routing_app satsim::app_inputs satsim::app_common satsim::failure satsim::network
    satsim::topology satsim::constellation satsim::ground satsim::orbit satsim::frames
    satsim::time)
# Headers are included as "failures/timeline.hpp".
target_include_directories(satsim_failures_app PUBLIC "${PROJECT_SOURCE_DIR}/apps")
target_link_libraries(satsim_failures_app PRIVATE satsim::core)
