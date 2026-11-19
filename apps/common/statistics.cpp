#include "common/statistics.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace satsim::app {

double percentile_of_sorted(const std::vector<double>& sorted, double p) {
  assert(!sorted.empty() && p >= 0.0 && p <= 100.0);
  const double rank = p / 100.0 * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(rank));
  const std::size_t upper = std::min(lower + 1, sorted.size() - 1);
  const double weight = rank - static_cast<double>(lower);
  return sorted[lower] + weight * (sorted[upper] - sorted[lower]);
}

std::optional<Quantiles> quantiles(std::vector<double> values) {
  if (values.empty()) {
    return std::nullopt;
  }
  std::sort(values.begin(), values.end());
  return Quantiles{
      .count = values.size(),
      .min = values.front(),
      .p5 = percentile_of_sorted(values, 5.0),
      .median = percentile_of_sorted(values, 50.0),
      .p95 = percentile_of_sorted(values, 95.0),
      .max = values.back(),
  };
}

std::map<std::int64_t, std::size_t> band_counts(const std::vector<double>& values,
                                                double band_width) {
  std::map<std::int64_t, std::size_t> counts;
  for (const double value : values) {
    ++counts[static_cast<std::int64_t>(std::floor(value / band_width))];
  }
  return counts;
}

}  // namespace satsim::app
