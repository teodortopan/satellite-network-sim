# Shortest paths over a weighted graph, and the pieces it falls into. Depends on nothing else
# of satsim: what nodes stand for is the caller's business.
satsim_add_library(network
  SOURCES graph.cpp path.cpp shortest_path.cpp many_sources.cpp paths_to.cpp pieces.cpp)
