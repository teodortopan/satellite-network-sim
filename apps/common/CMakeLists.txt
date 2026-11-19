# Helpers shared by the programs: CSV and number formatting, summary statistics, the time
# steps of a run and command-line parsing. Headers are included as "common/csv.hpp".
satsim_add_library(app_common
  SOURCES arguments.cpp csv.cpp statistics.cpp steps.cpp
  DEPS satsim::time)
target_include_directories(satsim_app_common PUBLIC "${PROJECT_SOURCE_DIR}/apps")
