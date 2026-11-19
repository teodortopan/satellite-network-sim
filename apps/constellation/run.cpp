#include "constellation/run.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
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

#include "constellation/arguments.hpp"
#include "propagate/csv.hpp"
#include "propagate/statistics.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/constellation/geometry.hpp"
#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/orbit/snapshot.hpp"

namespace satsim::constellation_app {

namespace {

using constellation::ClassifiedSatellite;
using constellation::ExclusionReason;
using constellation::PlaneAssignment;
using propagate::append_csv_field;
using propagate::append_double;
using propagate::append_integer;

constexpr int kTimeFractionDigits = 9;

// `value` with `decimals` digits after the point, locale-independent.
std::string fixed(double value, int decimals) {
  std::array<char, 64> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                       std::chars_format::fixed, decimals);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("?");
}

// The reference time as text, without trailing zero fraction digits.
std::string time_text(const time::UtcTime& utc) {
  std::string text = propagate::format_utc_iso(utc, kTimeFractionDigits);
  text.erase(text.find_last_not_of('0') + 1);
  if (text.back() == '.') {
    text.pop_back();
  }
  return text;
}

void append_angle_deg(std::string& line, double radians) {
  line.push_back(',');
  append_double(line, rad_to_deg(radians));
}

void write_line(std::ostream& csv, const std::string& line) {
  csv.write(line.data(), static_cast<std::streamsize>(line.size()));
}

void write_file_load(std::ostream& out, std::string_view name, const orbit::ElementFileLoad& load) {
  out << "  " << name << ": " << load.records_loaded << " records loaded, "
      << load.record_errors.size() << " record errors\n";
}

}  // namespace

ClassifiedRunOrError classify_snapshot(const Options& options,
                                       const constellation::ClassificationConfig& config) {
  constexpr std::array<std::string_view, 2> kFiles = {orbit::kStandardElementsFileName,
                                                      orbit::kSupplementalElementsFileName};
  if (std::optional<std::string> error =
          orbit::check_snapshot_directory(options.snapshot_dir, kFiles)) {
    return std::move(*error);
  }
  if (std::optional<std::string> error = constellation::check_config(config)) {
    return "configuration: " + *error;
  }
  orbit::SnapshotElementsOrError loaded = orbit::load_snapshot_elements(options.snapshot_dir);
  if (auto* error = std::get_if<std::string>(&loaded)) {
    return std::move(*error);
  }
  auto& elements = std::get<orbit::SnapshotElements>(loaded);

  ClassifiedRun run{
      .reference = options.reference,
      .config = config,
      .standard_file = std::move(elements.standard_file),
      .supplemental_file = std::move(elements.supplemental_file),
      .superseded = elements.selection.superseded.size(),
      .element_sets = std::move(elements.selection.selected),
      .classification = {},
  };
  std::vector<orbit::ElementSet> sets;
  sets.reserve(run.element_sets.size());
  for (const orbit::SourcedElementSet& s : run.element_sets) {
    sets.push_back(s.elements);
  }
  constellation::ClassificationOrError classified =
      constellation::classify_constellation(sets, options.reference, config);
  if (auto* error = std::get_if<std::string>(&classified)) {
    return "configuration: " + *error;  // Unreachable: checked above.
  }
  run.classification = std::move(std::get<constellation::Classification>(classified));
  return run;
}

bool write_csv(const ClassifiedRun& run, std::ostream& csv) {
  std::string line;
  for (const std::string_view column : kCsvColumns) {
    if (!line.empty()) {
      line.push_back(',');
    }
    line.append(column);
  }
  line.push_back('\n');
  write_line(csv, line);

  const std::vector<ClassifiedSatellite>& satellites = run.classification.satellites;
  for (std::size_t i = 0; i < satellites.size() && csv; ++i) {
    const ClassifiedSatellite& sat = satellites[i];
    // Both lists are in ascending catalogue number and hold the same satellites.
    const orbit::SourcedElementSet& chosen = run.element_sets[i];
    line.clear();
    append_integer(line, sat.norad_cat_id);
    line.push_back(',');
    append_csv_field(line, sat.object_name);
    line.push_back(',');
    append_csv_field(line, sat.object_id);
    line.push_back(',');
    line.append(chosen.source == orbit::ElementSource::kSupplemental ? "supplemental" : "standard");
    line.push_back(',');
    const std::optional<time::UtcTime> epoch = time::UtcTime::from_civil(chosen.elements.epoch);
    if (epoch) {
      append_double(line, run.reference.seconds_since(*epoch));
    }
    line.push_back(',');
    if (sat.shell_index) {
      append_csv_field(line, run.config.shells[*sat.shell_index].name);
    }
    line.push_back(',');
    if (const auto* place = std::get_if<PlaneAssignment>(&sat.placement)) {
      append_integer(line, static_cast<std::int64_t>(place->plane_index));
      line.push_back(',');
      append_integer(line, static_cast<std::int64_t>(place->index_in_plane));
      line.append(",,");
    } else {
      line.append(",,");
      line.append(constellation::name_of(std::get<ExclusionReason>(sat.placement)));
      line.push_back(',');
    }
    if (sat.propagation_error) {
      append_csv_field(line, orbit::describe(*sat.propagation_error));
    }
    if (const auto& g = sat.geometry) {
      append_angle_deg(line, g->inclination_rad);
      append_angle_deg(line, g->raan_teme_rad);
      append_angle_deg(line, g->argument_of_latitude_rad);
      line.push_back(',');
      append_double(line, g->mean_height_km);
      line.push_back(',');
      append_double(line, g->mean_radius_km);
      append_angle_deg(line, g->osculating_inclination_rad);
      append_angle_deg(line, g->osculating_raan_teme_rad);
      append_angle_deg(line, g->osculating_argument_of_latitude_rad);
      line.push_back(',');
      if (g->raan_drift_rad_per_day) {
        append_double(line, rad_to_deg(*g->raan_drift_rad_per_day));
      }
    } else {
      line.append(",,,,,,,,,");
    }
    line.push_back('\n');
    write_line(csv, line);
  }
  csv.flush();
  return static_cast<bool>(csv);
}

void write_summary(const ClassifiedRun& run, std::ostream& out) {
  const constellation::Classification& c = run.classification;
  out << "constellation summary\n";
  out << "  reference time: " << time_text(run.reference) << " UTC\n";
  write_file_load(out, orbit::kStandardElementsFileName, run.standard_file);
  write_file_load(out, orbit::kSupplementalElementsFileName, run.supplemental_file);
  out << "  element sets superseded: " << run.superseded << '\n';
  out << "  satellites: " << c.satellites.size() << '\n';

  std::vector<std::size_t> in_planes(run.config.shells.size(), 0);
  std::vector<std::size_t> in_band(run.config.shells.size(), 0);
  std::map<ExclusionReason, std::size_t> reasons;
  std::map<orbit::PropagationError, std::size_t> propagation_errors;
  for (const ClassifiedSatellite& sat : c.satellites) {
    if (sat.shell_index) {
      ++in_band[*sat.shell_index];
    }
    if (std::holds_alternative<PlaneAssignment>(sat.placement)) {
      if (sat.shell_index) {  // Always set for a satellite in a plane.
        ++in_planes[*sat.shell_index];
      }
    } else {
      ++reasons[std::get<ExclusionReason>(sat.placement)];
    }
    if (sat.propagation_error) {
      ++propagation_errors[*sat.propagation_error];
    }
  }
  std::size_t total_in_planes = 0;
  for (std::size_t s = 0; s < run.config.shells.size(); ++s) {
    const constellation::ShellDefinition& shell = run.config.shells[s];
    const std::vector<constellation::Plane>& planes = c.planes[s];
    total_in_planes += in_planes[s];
    out << "  shell " << shell.name << " (inclination " << fixed(shell.inclination_deg, 2) << " +- "
        << fixed(shell.inclination_tolerance_deg, 2) << " deg, height "
        << fixed(shell.min_height_km, 1) << " to " << fixed(shell.max_height_km, 1)
        << " km, plane gap " << fixed(shell.plane_gap_threshold_deg, 2) << " deg, planes of "
        << shell.min_plane_size << " or more): " << in_band[s] << " satellites in the bands, "
        << in_planes[s] << " in planes, " << planes.size() << " planes";
    std::vector<double> sizes;
    sizes.reserve(planes.size());
    for (const constellation::Plane& plane : planes) {
      sizes.push_back(static_cast<double>(plane.members.size()));
    }
    if (const std::optional<propagate::Quantiles> q = propagate::quantiles(sizes)) {
      out << ", plane size min " << fixed(q->min, 0) << " median " << fixed(q->median, 1) << " max "
          << fixed(q->max, 0);
    }
    out << '\n';
  }
  out << "  satellites in planes: " << total_in_planes << '\n';
  std::size_t excluded = 0;
  for (const auto& [reason, count] : reasons) {
    excluded += count;
  }
  out << "  satellites excluded: " << excluded << '\n';
  for (const ExclusionReason reason : constellation::kAllExclusionReasons) {
    const auto it = reasons.find(reason);
    out << "    " << constellation::name_of(reason) << ": "
        << (it == reasons.end() ? 0 : it->second) << '\n';
  }
  for (const auto& [error, count] : propagation_errors) {
    out << "      " << orbit::describe(error) << ": " << count << '\n';
  }
}

int run_main(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  const ParsedArguments parsed = parse_arguments(args);
  if (parsed.help) {
    out << usage();
    return 0;
  }
  if (!parsed.options) {
    err << "constellation: " << parsed.error.value_or("invalid arguments") << '\n'
        << "Run 'constellation --help' for usage.\n";
    return 2;
  }
  const Options& options = *parsed.options;
  ClassifiedRunOrError classified = classify_snapshot(options, constellation::default_config());
  if (const auto* error = std::get_if<std::string>(&classified)) {
    err << "constellation: " << *error << '\n';
    return 1;
  }
  const ClassifiedRun& run = std::get<ClassifiedRun>(classified);

  bool written = false;
  if (options.out == "-") {
    written = write_csv(run, out);
    write_summary(run, err);
  } else {
    std::ofstream file(options.out, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      err << "constellation: cannot open " << options.out << " for writing\n";
      return 1;
    }
    written = write_csv(run, file);
    file.close();
    written = written && static_cast<bool>(file);
    write_summary(run, out);
  }
  if (!written) {
    err << "constellation: writing the CSV failed (" << options.out << ")\n";
    return 1;
  }
  return 0;
}

}  // namespace satsim::constellation_app
