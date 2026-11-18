satsim_add_library(time SOURCES civil_time.cpp utc_time.cpp gmst.cpp time_scales.cpp earth_orientation.cpp
  DEPS satsim::core)
