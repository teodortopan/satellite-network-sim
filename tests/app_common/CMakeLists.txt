# The programs' shared helpers (apps/common).
satsim_add_test(satsim_app_common_tests
  SOURCES csv_test.cpp arguments_test.cpp
  DEPS satsim::app_common satsim::time)
