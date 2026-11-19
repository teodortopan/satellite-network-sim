satsim_add_test(satsim_ground_tests
  SOURCES address_block_test.cpp distance_test.cpp sites_test.cpp loaders_test.cpp
    home_pop_test.cpp real_data_test.cpp
  DEPS satsim::ground satsim::core)
