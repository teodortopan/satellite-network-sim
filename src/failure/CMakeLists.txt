satsim_add_library(failure
  SOURCES schedule.cpp failure_file.cpp detection.cpp
  DEPS satsim::topology satsim::constellation satsim::time)
