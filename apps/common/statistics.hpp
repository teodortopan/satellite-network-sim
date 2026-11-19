#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace satsim::app {

/// Order statistics of a sample, for the programs' summaries.
struct Quantiles {
  std::size_t count = 0;
  double min = 0.0;
  double p5 = 0.0;
  double median = 0.0;
  double p95 = 0.0;
  double max = 0.0;
};

/// The p-th percentile (p in [0, 100]) of an ascending, non-empty sample, interpolated
/// linearly between the two order statistics around rank p/100 * (n - 1) (Hyndman and Fan's
/// definition 7, numpy's default). The median of an even-sized sample is therefore the mean
/// of the middle two.
[[nodiscard]] double percentile_of_sorted(const std::vector<double>& sorted, double p);

/// Quantiles of `values` (in any order), or nullopt for an empty sample.
[[nodiscard]] std::optional<Quantiles> quantiles(std::vector<double> values);

/// Number of values in each band [k * band_width, (k + 1) * band_width), keyed by k. Bands
/// without values are absent.
[[nodiscard]] std::map<std::int64_t, std::size_t> band_counts(const std::vector<double>& values,
                                                              double band_width);

}  // namespace satsim::app
