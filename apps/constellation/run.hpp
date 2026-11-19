#pragma once

#include <array>
#include <cstddef>
#include <iosfwd>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "constellation/arguments.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/snapshot.hpp"

namespace satsim::constellation_app {

/// The CSV columns, in order. Angles in degrees, every name with its frame and unit; a field
/// is empty when it does not apply:
///  - norad_cat_id, object_name, object_id (CSV-quoted when needed), element_source
///    ("supplemental" or "standard"), element_age_s (reference time minus the epoch);
///  - shell: the shell whose inclination and height bands hold the geometry (any reason);
///  - plane_index, index_in_plane: set exactly when exclusion_reason is empty;
///  - exclusion_reason: a name_of(ExclusionReason); propagation_error: SGP4's error, set
///    exactly when the reason is propagation_failed;
///  - the geometry (OrbitGeometry), empty when propagation failed: inclination_deg,
///    raan_teme_deg, argument_of_latitude_deg (mean over one orbit, TEME, true equator of
///    date), mean_height_km (WGS-84 geodetic), mean_radius_km, the same three angles
///    osculating at the reference time, and raan_drift_deg_per_day (empty when the drift
///    window could not be propagated).
inline constexpr std::array<std::string_view, 19> kCsvColumns = {
    "norad_cat_id",
    "object_name",
    "object_id",
    "element_source",
    "element_age_s",
    "shell",
    "plane_index",
    "index_in_plane",
    "exclusion_reason",
    "propagation_error",
    "inclination_deg",
    "raan_teme_deg",
    "argument_of_latitude_deg",
    "mean_height_km",
    "mean_radius_km",
    "osculating_inclination_deg",
    "osculating_raan_teme_deg",
    "osculating_argument_of_latitude_deg",
    "raan_drift_deg_per_day",
};

/// A classified snapshot, ready to write.
struct ClassifiedRun {
  time::UtcTime reference;
  constellation::ClassificationConfig config;
  orbit::ElementFileLoad standard_file;
  orbit::ElementFileLoad supplemental_file;
  std::size_t superseded = 0;
  /// The chosen element sets, ascending catalogue number (the order of the classification).
  std::vector<orbit::SourcedElementSet> element_sets;
  constellation::Classification classification;
};

/// A classified run, or why it could not be done.
using ClassifiedRunOrError = std::variant<ClassifiedRun, std::string>;

/// Loads the snapshot and classifies it with `config`. Fails, with a message naming the path,
/// if the directory or one of its two element files is missing or cannot be loaded as a
/// whole, or if `config` is not usable.
[[nodiscard]] ClassifiedRunOrError classify_snapshot(
    const Options& options, const constellation::ClassificationConfig& config);

/// Writes the CSV header and one row per satellite, ascending catalogue number. Returns false
/// if the stream fails.
[[nodiscard]] bool write_csv(const ClassifiedRun& run, std::ostream& csv);

/// Writes the human-readable summary: inputs, satellites per shell, planes per shell with the
/// smallest, median and largest plane, and exclusions by reason.
void write_summary(const ClassifiedRun& run, std::ostream& out);

/// The whole program with the default configuration, minus opening the standard streams:
/// parses `args` (without the program name), classifies, writes the CSV to the --out file or
/// to `out` for "-", and the summary to `out` (to `err` when the CSV goes to `out`). Messages
/// go to `err`. Returns 0 on success, 1 if the run fails, 2 for invalid arguments. A file
/// named by --out is only created once the snapshot has been classified.
[[nodiscard]] int run_main(std::span<const std::string_view> args, std::ostream& out,
                           std::ostream& err);

}  // namespace satsim::constellation_app
