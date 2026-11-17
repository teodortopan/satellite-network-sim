#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "satsim/orbit/element_set.hpp"

namespace satsim::orbit {

/// Why one record of an OMM JSON array was rejected.
struct RecordError {
  std::size_t index = 0;    ///< Position of the record in the top-level array, from 0.
  std::string object_name;  ///< OBJECT_NAME if it could be read, else empty.
  std::string object_id;    ///< OBJECT_ID if it could be read, else empty.
  std::optional<std::int64_t> norad_cat_id;  ///< NORAD_CAT_ID if it could be read.
  /// Every problem found in the record, joined by "; ", e.g. "ECCENTRICITY missing".
  std::string message;
};

/// The outcome of loading an OMM JSON file. A bad record never discards the good ones: the
/// good ones are in element_sets (in file order) and each bad one is in record_errors.
struct LoadResult {
  std::vector<ElementSet> element_sets;
  std::vector<RecordError> record_errors;
  /// Set when the input as a whole could not be used (unreadable, not JSON, top-level value
  /// not an array). The two vectors are then empty.
  std::optional<std::string> file_error;
};

/// Loads CelesTrak OMM JSON: a top-level array of flat objects. Required per record:
/// NORAD_CAT_ID, EPOCH, MEAN_MOTION, ECCENTRICITY, INCLINATION, RA_OF_ASC_NODE,
/// ARG_OF_PERICENTER, MEAN_ANOMALY and BSTAR (everything SGP4 needs). Optional:
/// OBJECT_NAME, OBJECT_ID, MEAN_MOTION_DOT, MEAN_MOTION_DDOT, and EPHEMERIS_TYPE, which
/// must be 0 (plain SGP4) when present. Other keys are ignored. Never throws.
[[nodiscard]] LoadResult load_omm_json(std::istream& in) noexcept;

/// Opens path and loads it as above.
[[nodiscard]] LoadResult load_omm_json(const std::filesystem::path& path) noexcept;

}  // namespace satsim::orbit
