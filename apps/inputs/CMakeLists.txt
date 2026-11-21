# What the network programs (topology, routing) share: loading and checking the snapshot, the
# ground data and the city pairs (inputs.hpp), the options that choose them
# (input_arguments.hpp), and stepping through time with an optional look-ahead
# (step_source.hpp). Headers are included as "inputs/inputs.hpp".
satsim_add_library(app_inputs
  SOURCES inputs.cpp input_arguments.cpp step_source.cpp
  DEPS satsim::app_common satsim::topology satsim::constellation satsim::ground satsim::orbit
    satsim::time)
target_include_directories(satsim_app_inputs PUBLIC "${PROJECT_SOURCE_DIR}/apps")
target_link_libraries(satsim_app_inputs PRIVATE satsim::core)
