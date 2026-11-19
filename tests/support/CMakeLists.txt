# Code shared by several test executables. synthetic.cpp builds ideal Walker shells; it reads
# SGP4's secular rates from Vallado's sgp4init (hence the vendored library and src/orbit's
# internal header) to move an element set to another epoch exactly. Tests include it as
# "support/synthetic.hpp".
satsim_add_library(test_support
  SOURCES synthetic.cpp
  DEPS satsim::orbit satsim::sgp4 satsim::time satsim::core GTest::gtest)
target_include_directories(satsim_test_support
  PUBLIC "${PROJECT_SOURCE_DIR}/tests"
  PRIVATE "${PROJECT_SOURCE_DIR}/src/orbit")
