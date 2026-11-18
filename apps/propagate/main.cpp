// propagate: a saved snapshot and a time range in, satellite positions out as CSV. All the
// logic is in the satsim_propagate_app library (run.hpp), where the tests reach it.

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "propagate/run.hpp"

int main(int argc, char** argv) {
  try {
    // Standard output may carry the whole CSV; C stdio is never used here.
    std::ios::sync_with_stdio(false);
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::span<char*> given = raw.subspan(raw.empty() ? 0 : 1);  // Without the name.
    const std::vector<std::string_view> args(given.begin(), given.end());
    return satsim::propagate::run_main(args, std::cout, std::cerr);
  } catch (const std::exception& e) {
    std::cerr << "propagate: " << e.what() << '\n';
    return 1;
  }
}
