#include "propagate/run.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "propagate/arguments.hpp"
#include "propagate/csv.hpp"
#include "propagate/statistics.hpp"
#include "satsim/core/angles.hpp"
#include "satsim/frames/geodetic.hpp"
#include "satsim/frames/teme_ecef.hpp"
#include "satsim/frames/teme_j2000.hpp"
#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/omm_loader.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/time_scales.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::propagate {

namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr double kSecondsPerDay = 86400.0;
constexpr std::size_t kMaxRecordErrorsListed = 10;

std::string_view source_name(orbit::ElementSource source) noexcept {
  return source == orbit::ElementSource::kSupplemental ? "supplemental" : "standard";
}

// The date (YYYY-MM-DD) of a Modified Julian Date. MJD 0 is 1858-11-17.
std::string date_of_mjd(std::int64_t mjd) {
  const std::optional<time::UtcTime> mjd_zero =
      time::UtcTime::from_civil({1858, 11, 17, 0, 0, 0.0});
  if (!mjd_zero) {
    return "?";  // Unreachable: the date above exists.
  }
  return format_utc_iso(mjd_zero->plus_seconds(static_cast<double>(mjd) * kSecondsPerDay), 0)
      .substr(0, 10);
}

// `value` with `decimals` digits after the point, locale-independent.
std::string fixed(double value, int decimals) {
  std::array<char, 64> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                       std::chars_format::fixed, decimals);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("?");
}

std::string seconds_text(std::int64_t ns) {
  std::string text;
  append_integer(text, ns / kNanosecondsPerSecond);
  std::int64_t fraction = ns % kNanosecondsPerSecond;
  if (fraction != 0) {
    // Nine digits, then trailing zeros dropped.
    std::string digits = std::to_string(fraction);
    digits.insert(0, 9 - digits.size(), '0');
    digits.erase(digits.find_last_not_of('0') + 1);
    text += "." + digits;
  }
  return text;
}

FileLoadSummary summarise_load(const orbit::LoadResult& load) {
  return {.records_loaded = load.element_sets.size(), .record_errors = load.record_errors};
}

// Loads one OMM JSON file of the snapshot; a file-level error is fatal.
std::variant<orbit::LoadResult, std::string> load_elements(const std::filesystem::path& path) {
  orbit::LoadResult load = orbit::load_omm_json(path);
  if (load.file_error) {
    return *load.file_error;  // Already names the path.
  }
  return load;
}

std::string earth_orientation_gap_message(const std::string& time_text,
                                          const std::filesystem::path& path,
                                          const time::EarthOrientationTable& table) {
  return "no Earth-orientation values for " + time_text + ": " + path.string() + " covers " +
         date_of_mjd(table.first_mjd()) + "T00:00:00 to " + date_of_mjd(table.last_mjd()) +
         "T00:00:00 (a leap second between two rows also leaves a gap); values are never "
         "extrapolated";
}

void write_header(std::ostream& csv) {
  std::string line;
  for (const std::string_view column : kCsvColumns) {
    if (!line.empty()) {
      line.push_back(',');
    }
    line.append(column);
  }
  line.push_back('\n');
  csv.write(line.data(), static_cast<std::streamsize>(line.size()));
}

void append_vector(std::string& line, const Eigen::Vector3d& v) {
  for (Eigen::Index i = 0; i < 3; ++i) {
    line.push_back(',');
    append_double(line, v[i]);
  }
}

void write_count_map(std::ostream& out,
                     const std::map<orbit::PropagationError, std::size_t>& counts) {
  for (const auto& [error, count] : counts) {
    out << "    " << orbit::describe(error) << ": " << count << '\n';
  }
}

void write_file_load(std::ostream& out, std::string_view name, const FileLoadSummary& load) {
  out << "  " << name << ": " << load.records_loaded << " records loaded, "
      << load.record_errors.size() << " record errors\n";
  const std::size_t listed = std::min(load.record_errors.size(), kMaxRecordErrorsListed);
  for (std::size_t i = 0; i < listed; ++i) {
    const orbit::RecordError& e = load.record_errors[i];
    out << "    record " << e.index;
    if (e.norad_cat_id) {
      out << " (" << *e.norad_cat_id << (e.object_name.empty() ? "" : " " + e.object_name) << ")";
    }
    out << ": " << e.message << '\n';
  }
  if (listed < load.record_errors.size()) {
    out << "    ... and " << load.record_errors.size() - listed << " more\n";
  }
}

void write_ages(std::ostream& out, std::string_view source, const std::vector<double>& ages_s) {
  out << "  element age at start, " << source << ": ";
  const std::optional<Quantiles> q = quantiles(ages_s);
  if (!q) {
    out << "no satellites\n";
    return;
  }
  const auto hours = [](double s) { return fixed(s / 3600.0, 2); };
  out << q->count << " satellites, min " << hours(q->min) << " h, median " << hours(q->median)
      << " h, max " << hours(q->max) << " h\n";
}

}  // namespace

time::UtcTime time_of_step(const time::UtcTime& start, std::int64_t step_ns,
                           std::int64_t index) noexcept {
  const std::int64_t offset_ns = index * step_ns;
  // Whole seconds (at most 1e9, exact in a double) then the rest (below 1e9 ns, so the
  // quotient by 1e9 is within 1e-16 s of it and plus_seconds rounds it back exactly).
  const std::int64_t whole_s = offset_ns / kNanosecondsPerSecond;
  const std::int64_t rest_ns = offset_ns % kNanosecondsPerSecond;
  return start.plus_seconds(static_cast<double>(whole_s))
      .plus_seconds(static_cast<double>(rest_ns) / static_cast<double>(kNanosecondsPerSecond));
}

PreparedRunOrError prepare_run(const Options& options) {
  namespace fs = std::filesystem;
  // parse_arguments guarantees these; checked again for Options built by hand.
  if (options.step_ns <= 0 || options.duration_ns < 0 || options.duration_ns > kMaxDurationNs) {
    return std::string("step must be positive and duration in [0, 1e9] s");
  }
  const fs::path& dir = options.snapshot_dir;
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return "snapshot directory " + dir.string() + " does not exist";
  }
  if (!fs::is_directory(dir, ec)) {
    return "snapshot " + dir.string() + " is not a directory";
  }
  std::string missing;
  for (const std::string_view name :
       {kStandardFileName, kSupplementalFileName, kEarthOrientationFileName}) {
    if (!fs::is_regular_file(dir / name, ec)) {
      missing += (missing.empty() ? "" : ", ") + std::string(name);
    }
  }
  if (!missing.empty()) {
    return "snapshot directory " + dir.string() + " has no " + missing;
  }

  // Earth orientation first: it is quick to load, and the time range can be checked before
  // the element files are parsed.
  const fs::path eop_path = dir / kEarthOrientationFileName;
  time::EarthOrientationLoadResult eop = time::load_earth_orientation(eop_path);
  if (!eop.table) {
    return eop.error.value_or(eop_path.string() + ": could not be loaded");
  }

  PreparedRun run{
      .start = options.start,
      .step_ns = options.step_ns,
      .time_count = options.duration_ns / options.step_ns + 1,
      .fraction_digits = fraction_digits_for(options.start, options.step_ns),
      .earth_orientation = std::move(*eop.table),
      .satellites = {},
      .summary = {},
  };
  RunSummary& summary = run.summary;
  summary.earth_orientation_first_mjd = run.earth_orientation.first_mjd();
  summary.earth_orientation_last_mjd = run.earth_orientation.last_mjd();
  summary.step_ns = run.step_ns;
  summary.time_count = run.time_count;
  const time::UtcTime last = time_of_step(run.start, run.step_ns, run.time_count - 1);
  summary.first_time = format_utc_iso(run.start, run.fraction_digits);
  summary.last_time = format_utc_iso(last, run.fraction_digits);

  // Both ends first, so a range far outside the table fails at once; then every time, for a
  // gap inside the table.
  for (const time::UtcTime& t : {run.start, last}) {
    if (!run.earth_orientation.at(t)) {
      return earth_orientation_gap_message(format_utc_iso(t, run.fraction_digits), eop_path,
                                           run.earth_orientation);
    }
  }
  for (std::int64_t i = 1; i + 1 < run.time_count; ++i) {
    const time::UtcTime t = time_of_step(run.start, run.step_ns, i);
    if (!run.earth_orientation.at(t)) {
      return earth_orientation_gap_message(format_utc_iso(t, run.fraction_digits), eop_path,
                                           run.earth_orientation);
    }
  }

  auto standard = load_elements(dir / kStandardFileName);
  if (const auto* error = std::get_if<std::string>(&standard)) {
    return *error;
  }
  auto supplemental = load_elements(dir / kSupplementalFileName);
  if (const auto* error = std::get_if<std::string>(&supplemental)) {
    return *error;
  }
  const orbit::LoadResult& standard_load = std::get<orbit::LoadResult>(standard);
  const orbit::LoadResult& supplemental_load = std::get<orbit::LoadResult>(supplemental);
  summary.standard_file = summarise_load(standard_load);
  summary.supplemental_file = summarise_load(supplemental_load);

  orbit::ElementSelection selection =
      orbit::select_element_sets(standard_load.element_sets, supplemental_load.element_sets);
  summary.superseded = selection.superseded.size();

  const auto by_id = [](const orbit::SourcedElementSet& s, std::int64_t id) {
    return s.elements.norad_cat_id < id;
  };
  for (const std::int64_t id : options.ids) {
    const auto it =
        std::lower_bound(selection.selected.begin(), selection.selected.end(), id, by_id);
    if (it == selection.selected.end() || it->elements.norad_cat_id != id) {
      summary.ids_not_found.push_back(id);
    }
  }

  for (orbit::SourcedElementSet& chosen : selection.selected) {
    if (!options.ids.empty() &&
        !std::binary_search(options.ids.begin(), options.ids.end(), chosen.elements.norad_cat_id)) {
      continue;
    }
    const bool supplemental_set = chosen.source == orbit::ElementSource::kSupplemental;
    ++(supplemental_set ? summary.selected_supplemental : summary.selected_standard);
    orbit::PropagatorOrError built = orbit::Propagator::create(chosen.elements);
    if (const auto* error = std::get_if<orbit::PropagationError>(&built)) {
      ++summary.build_failures[*error];
      continue;
    }
    auto& propagator = std::get<orbit::Propagator>(built);
    const double age_s = run.start.seconds_since(propagator.epoch());
    (supplemental_set ? summary.supplemental_age_at_start_s : summary.standard_age_at_start_s)
        .push_back(age_s);
    run.satellites.push_back(
        {.element_set = std::move(chosen), .propagator = std::move(propagator)});
  }
  return run;
}

RunOutcome write_rows(const PreparedRun& run, std::ostream& csv) {
  RunOutcome outcome{.error = std::nullopt, .summary = run.summary};
  RunSummary& summary = outcome.summary;
  std::vector<bool> failed(run.satellites.size(), false);

  write_header(csv);
  std::string line;
  for (std::int64_t i = 0; i < run.time_count && csv; ++i) {
    const time::UtcTime utc = time_of_step(run.start, run.step_ns, i);
    const std::string time_text = format_utc_iso(utc, run.fraction_digits);
    // prepare_run checked every time against the table, so this never fails; if it did, the
    // run stops rather than use zero corrections.
    const std::optional<time::EarthOrientation> eop = run.earth_orientation.at(utc);
    if (!eop) {
      outcome.error = "no Earth-orientation values for " + time_text;
      break;
    }
    // The same matrix frames::teme_to_j2000 builds, computed once per time, not per satellite.
    const Eigen::Matrix3d teme_to_j2000 = frames::teme_to_j2000_matrix(time::tt_from_utc(utc));

    for (std::size_t s = 0; s < run.satellites.size(); ++s) {
      const Satellite& satellite = run.satellites[s];
      const orbit::StateOrError result = satellite.propagator.propagate(utc);
      if (const auto* error = std::get_if<orbit::PropagationError>(&result)) {
        ++summary.propagation_failures[*error];
        failed[s] = true;
        continue;
      }
      const auto& teme = std::get<orbit::TemeState>(result);
      const Eigen::Vector3d position_ecef_km = frames::teme_to_ecef(teme.position_km, utc, *eop);
      const frames::Geodetic geodetic = frames::ecef_to_geodetic(position_ecef_km);
      const Eigen::Vector3d position_j2000_km = teme_to_j2000 * teme.position_km;
      const Eigen::Vector3d velocity_j2000_km_s = teme_to_j2000 * teme.velocity_km_s;
      if (i == 0) {
        summary.height_at_start_km.push_back(geodetic.height_km);
      }

      const orbit::ElementSet& elements = satellite.element_set.elements;
      line.clear();
      line.append(time_text);
      line.push_back(',');
      append_integer(line, elements.norad_cat_id);
      line.push_back(',');
      append_csv_field(line, elements.object_name);
      line.push_back(',');
      line.append(source_name(satellite.element_set.source));
      line.push_back(',');
      append_double(line, utc.seconds_since(satellite.propagator.epoch()));
      append_vector(line, teme.position_km);
      append_vector(line, teme.velocity_km_s);
      append_vector(line, position_ecef_km);
      line.push_back(',');
      append_double(line, rad_to_deg(geodetic.latitude_rad));
      line.push_back(',');
      append_double(line, rad_to_deg(geodetic.longitude_rad));
      line.push_back(',');
      append_double(line, geodetic.height_km);
      append_vector(line, position_j2000_km);
      append_vector(line, velocity_j2000_km_s);
      line.push_back('\n');
      csv.write(line.data(), static_cast<std::streamsize>(line.size()));
      ++summary.rows_written;
    }
  }
  csv.flush();
  summary.satellites_with_propagation_failures =
      static_cast<std::size_t>(std::count(failed.begin(), failed.end(), true));
  if (!csv && !outcome.error) {
    outcome.error = "writing the CSV failed";
  }
  return outcome;
}

RunOutcome run(const Options& options, std::ostream& csv) {
  PreparedRunOrError prepared = prepare_run(options);
  if (auto* error = std::get_if<std::string>(&prepared)) {
    return {.error = std::move(*error), .summary = {}};
  }
  return write_rows(std::get<PreparedRun>(prepared), csv);
}

void write_summary(const RunSummary& summary, std::ostream& out) {
  out << "propagate summary\n";
  out << "  times: " << summary.time_count << ", from " << summary.first_time << " to "
      << summary.last_time << " UTC every " << seconds_text(summary.step_ns) << " s\n";
  out << "  Earth orientation: table covers " << date_of_mjd(summary.earth_orientation_first_mjd)
      << " to " << date_of_mjd(summary.earth_orientation_last_mjd) << " (MJD "
      << summary.earth_orientation_first_mjd << " to " << summary.earth_orientation_last_mjd
      << "), used from " << summary.first_time << " to " << summary.last_time << '\n';
  write_file_load(out, kStandardFileName, summary.standard_file);
  write_file_load(out, kSupplementalFileName, summary.supplemental_file);
  out << "  satellites selected: " << summary.selected_supplemental + summary.selected_standard
      << " (supplemental " << summary.selected_supplemental << ", standard "
      << summary.selected_standard << ")\n";
  if (!summary.ids_not_found.empty()) {
    out << "  requested ids not in the snapshot: " << summary.ids_not_found.size() << " (";
    for (std::size_t i = 0; i < summary.ids_not_found.size(); ++i) {
      out << (i == 0 ? "" : ", ") << summary.ids_not_found[i];
    }
    out << ")\n";
  }
  out << "  element sets superseded: " << summary.superseded << '\n';
  std::size_t build_failures = 0;
  for (const auto& [error, count] : summary.build_failures) {
    build_failures += count;
  }
  out << "  propagators that failed to build: " << build_failures << '\n';
  write_count_map(out, summary.build_failures);
  std::size_t propagation_failures = 0;
  for (const auto& [error, count] : summary.propagation_failures) {
    propagation_failures += count;
  }
  out << "  propagation failures: " << propagation_failures << " (satellite-times), "
      << summary.satellites_with_propagation_failures << " satellites\n";
  write_count_map(out, summary.propagation_failures);
  out << "  rows written: " << summary.rows_written << '\n';
  write_ages(out, "supplemental", summary.supplemental_age_at_start_s);
  write_ages(out, "standard", summary.standard_age_at_start_s);

  const std::optional<Quantiles> heights = quantiles(summary.height_at_start_km);
  out << "  geodetic height at start: ";
  if (!heights) {
    out << "no satellites\n";
    return;
  }
  const auto km = [](double h) { return fixed(h, 1); };
  out << heights->count << " satellites, min " << km(heights->min) << " km, p5 " << km(heights->p5)
      << " km, median " << km(heights->median) << " km, p95 " << km(heights->p95) << " km, max "
      << km(heights->max) << " km\n";
  out << "  satellites per " << fixed(kHeightBandKm, 0) << " km height band at start:\n";
  for (const auto& [band, count] : band_counts(summary.height_at_start_km, kHeightBandKm)) {
    out << "    [" << fixed(static_cast<double>(band) * kHeightBandKm, 0) << ", "
        << fixed(static_cast<double>(band + 1) * kHeightBandKm, 0) << ") km: " << count << '\n';
  }
}

int run_main(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  const ParsedArguments parsed = parse_arguments(args);
  if (parsed.help) {
    out << usage();
    return 0;
  }
  if (!parsed.options) {
    err << "propagate: " << parsed.error.value_or("invalid arguments") << '\n'
        << "Run 'propagate --help' for usage.\n";
    return 2;
  }
  const Options& options = *parsed.options;

  PreparedRunOrError prepared = prepare_run(options);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    err << "propagate: " << *error << '\n';
    return 1;
  }
  const PreparedRun& run = std::get<PreparedRun>(prepared);

  RunOutcome outcome;
  if (options.out == "-") {
    outcome = write_rows(run, out);
    write_summary(outcome.summary, err);
  } else {
    std::ofstream file(options.out, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      err << "propagate: cannot open " << options.out << " for writing\n";
      return 1;
    }
    outcome = write_rows(run, file);
    file.close();
    if (!file && !outcome.error) {
      outcome.error = "writing the CSV failed";
    }
    write_summary(outcome.summary, out);
  }
  if (outcome.error) {
    err << "propagate: " << *outcome.error << " (" << options.out << ")\n";
    return 1;
  }
  return 0;
}

}  // namespace satsim::propagate
