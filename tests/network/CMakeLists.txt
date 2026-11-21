satsim_add_test(satsim_network_tests
  SOURCES graph_test.cpp path_test.cpp shortest_path_test.cpp many_sources_test.cpp
          paths_to_test.cpp pieces_test.cpp
  DEPS satsim::network)
