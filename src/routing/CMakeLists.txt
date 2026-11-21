# Route strategies and the state of one held route, as pure graph logic. Depends on network
# only: it knows nothing about orbits, topology or the ground.
satsim_add_library(routing
  SOURCES strategy.cpp planner.cpp change.cpp forwarding.cpp
  DEPS satsim::network)
