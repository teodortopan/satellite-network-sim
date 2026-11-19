# The constellation program. Its logic is a library, so tests call it directly and it gets our
# warnings and sanitizers; the executable is only main(). It reuses the propagate program's CSV
# and statistics helpers (satsim::propagate_app), so both programs write numbers and fields the
# same way.
satsim_add_library(constellation_app
  SOURCES arguments.cpp run.cpp
  DEPS satsim::constellation satsim::orbit satsim::time)
# Headers are included as "constellation/run.hpp".
target_include_directories(satsim_constellation_app PUBLIC "${PROJECT_SOURCE_DIR}/apps")
target_link_libraries(satsim_constellation_app PRIVATE satsim::propagate_app satsim::core)

# Output in <build>/bin.
add_executable(constellation main.cpp)
target_link_libraries(constellation PRIVATE satsim::constellation_app)
set_target_properties(constellation PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
satsim_set_warnings(constellation)
satsim_enable_sanitizers(constellation)
