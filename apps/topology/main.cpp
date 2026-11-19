// topology: a saved snapshot, the ground data and settings in, the delay between city pairs at
// every time step out as CSV. All the logic is in the satsim_topology_app library (run.hpp),
// where the tests reach it.

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "topology/run.hpp"

int main(int argc, char** argv) {
  try {
    std::ios::sync_with_stdio(false);
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::span<char*> given = raw.subspan(raw.empty() ? 0 : 1);  // Without the name.
    const std::vector<std::string_view> args(given.begin(), given.end());
    return satsim::topology_app::run_main(args, std::cout, std::cerr);
  } catch (const std::exception& e) {
    std::cerr << "topology: " << e.what() << '\n';
    return 1;
  }
}
