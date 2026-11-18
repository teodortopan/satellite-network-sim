# The propagate program. Its logic is a library, so tests call it directly and it gets our
# warnings and sanitizers; the executable is only main().
satsim_add_library(propagate_app
  SOURCES arguments.cpp csv.cpp statistics.cpp run.cpp
  DEPS satsim::orbit satsim::frames satsim::time)
# Headers are included as "propagate/run.hpp".
target_include_directories(satsim_propagate_app PUBLIC "${PROJECT_SOURCE_DIR}/apps")
target_link_libraries(satsim_propagate_app PRIVATE satsim::core)

# Built by every preset, so the sanitizer builds also run it (tests/propagate's end-to-end
# test). Output in <build>/bin.
add_executable(propagate main.cpp)
target_link_libraries(propagate PRIVATE satsim::propagate_app)
set_target_properties(propagate PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
satsim_set_warnings(propagate)
satsim_enable_sanitizers(propagate)
