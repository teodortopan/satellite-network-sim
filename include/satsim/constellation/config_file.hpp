#pragma once

#include <filesystem>
#include <istream>
#include <string>
#include <variant>

#include "satsim/constellation/classification.hpp"

namespace satsim::constellation {

/// A classification configuration read from a file, or why it could not be read.
using ClassificationConfigOrError = std::variant<ClassificationConfig, std::string>;

/// Reads shell definitions (and optionally excluded groups) from a JSON object:
///
///     {"shells": [{"name": "s1", "inclination_deg": 53.0, "inclination_tolerance_deg": 0.3,
///                  "min_height_km": 500.0, "max_height_km": 530.0,
///                  "plane_gap_threshold_deg": 1.0, "min_plane_size": 10}],
///      "excluded_groups": [{"name": "g", "reason": "low_group", "min_inclination_deg": 0.0,
///                           "max_inclination_deg": 180.0, "min_height_km": 350.0,
///                           "max_height_km": 375.0}]}
///
/// Every shell key is required. "excluded_groups" is optional (absent: none), and each group
/// needs every key; "reason" is the name of an ExclusionReason (name_of). The result is
/// default_config() with its shells and excluded groups replaced; the launch without lasers
/// and the geometry sampling keep their defaults. Fails, naming the record and key, on a file
/// that is not a JSON object, an unknown key (so a misspelt key is never silently ignored), a
/// missing key, a value of the wrong type, no shell at all, two shells or groups with the
/// same name, or any value check_config rejects.
[[nodiscard]] ClassificationConfigOrError parse_classification_config(std::istream& in);

/// parse_classification_config on the file at `path`; errors are prefixed with the path.
[[nodiscard]] ClassificationConfigOrError load_classification_config(
    const std::filesystem::path& path);

}  // namespace satsim::constellation
