# Shortest paths over a weighted graph. Depends on nothing else of satsim: what nodes stand
# for is the caller's business.
satsim_add_library(network
  SOURCES graph.cpp path.cpp shortest_path.cpp)
