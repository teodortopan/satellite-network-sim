satsim_add_test(satsim_routing_tests
  SOURCES strategy_test.cpp planner_test.cpp change_test.cpp
  DEPS satsim::routing satsim::network)
