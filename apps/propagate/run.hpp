#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "propagate/arguments.hpp"
#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/omm_loader.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/orbit/snapshot.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::propagate {

/// The files a snapshot directory must hold (written by python/fetch_elements.py).
inline constexpr std::string_view kStandardFileName = orbit::kStandardElementsFileName;
inline constexpr std::string_view kSupplementalFileName = orbit::kSupplementalElementsFileName;
inline constexpr std::string_view kEarthOrientationFileName = orbit::kEarthOrientationFileName;

/// Width of the height bands the summary counts satellites in.
inline constexpr double kHeightBandKm = 50.0;

/// What loading one OMM JSON file gave.
using FileLoadSummary = orbit::ElementFileLoad;

/// Everything the summary reports. prepare_run fills the loading part, write_rows the rest.
struct RunSummary {
  FileLoadSummary standard_file;
  FileLoadSummary supplemental_file;

  /// The Earth-orientation table's first and last day (MJD, rows at 0h UTC).
  std::int64_t earth_orientation_first_mjd = 0;
  std::int64_t earth_orientation_last_mjd = 0;

  /// The output times: time_count of them from first_time to last_time, every step_ns.
  std::string first_time;
  std::string last_time;
  std::int64_t step_ns = 0;
  std::int64_t time_count = 0;

  /// Satellites selected (after --ids), by the source of their element set. Includes those
  /// whose propagator failed to build.
  std::size_t selected_supplemental = 0;
  std::size_t selected_standard = 0;
  /// Element sets passed over for a later one of the same satellite in the same file.
  std::size_t superseded = 0;
  /// --ids entries that are in neither file, ascending.
  std::vector<std::int64_t> ids_not_found;

  /// Satellites whose propagator could not be built, by error. They produce no rows.
  std::map<orbit::PropagationError, std::size_t> build_failures;
  /// Failed propagations (one per satellite and time), by error. They produce no rows.
  std::map<orbit::PropagationError, std::size_t> propagation_failures;
  /// Satellites with at least one failed propagation.
  std::size_t satellites_with_propagation_failures = 0;
  std::size_t rows_written = 0;

  /// Element age (start time minus epoch) of every satellite with a propagator, in seconds.
  std::vector<double> supplemental_age_at_start_s;
  std::vector<double> standard_age_at_start_s;
  /// Geodetic height at the start time of every satellite that propagated there, in km.
  std::vector<double> height_at_start_km;
};

/// One satellite of a run: its chosen element set and the propagator built from it.
using Satellite = orbit::SnapshotSatellite;

/// A run whose inputs are loaded and checked: write_rows can no longer fail on them.
struct PreparedRun {
  time::UtcTime start;
  std::int64_t step_ns = 0;
  std::int64_t time_count = 0;  ///< Number of output times, at least 1.
  int fraction_digits = 0;      ///< Fraction digits of the utc column (fraction_digits_for).
  time::EarthOrientationTable earth_orientation;  ///< Covers every output time.
  std::vector<Satellite> satellites;              ///< Ascending catalogue number.
  RunSummary summary;                             ///< The loading part.
};

/// A prepared run, or why the run cannot be done.
using PreparedRunOrError = std::variant<PreparedRun, std::string>;

/// The outcome of writing a run: the summary, and an error if writing the CSV failed.
struct RunOutcome {
  std::optional<std::string> error;
  RunSummary summary;
};

/// Loads and checks everything a run needs, writing nothing. Fails (with a message naming
/// the file or time at fault) if the snapshot directory or one of its three files is
/// missing, a file cannot be loaded as a whole (record-level errors are only counted), or
/// any output time has no Earth-orientation values (they are never extrapolated). Every
/// output time is checked before anything is written. Satellites whose propagator cannot be
/// built are counted in the summary and left out.
[[nodiscard]] PreparedRunOrError prepare_run(const Options& options);

/// Writes the CSV header and one row per satellite and time, ordered by time and then
/// catalogue number, streaming (memory does not grow with the number of times). A failed
/// propagation gives no row; it is counted. Returns an error only if the stream fails.
[[nodiscard]] RunOutcome write_rows(const PreparedRun& run, std::ostream& csv);

/// prepare_run then write_rows. On a preparation error nothing is written to `csv`.
[[nodiscard]] RunOutcome run(const Options& options, std::ostream& csv);

/// Writes the human-readable summary of a run.
void write_summary(const RunSummary& summary, std::ostream& out);

/// The whole program, minus opening the standard streams: parses `args` (without the program
/// name), runs, writes the CSV to the --out file or to `out` for "-", and the summary to `out`
/// (to `err` when the CSV goes to `out`). Messages go to `err`. Returns the exit status: 0
/// on success, 1 if the run fails, 2 for invalid arguments. A file named by --out is only
/// created once the inputs have been checked.
[[nodiscard]] int run_main(std::span<const std::string_view> args, std::ostream& out,
                           std::ostream& err);

}  // namespace satsim::propagate
