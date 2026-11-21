#include "topology/run.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/csv.hpp"
#include "common/statistics.hpp"
#include "common/steps.hpp"
#include "inputs/inputs.hpp"
#include "inputs/step_source.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/constellation/config_file.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/ground/home_pop.hpp"
#include "satsim/ground/loaders.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/snapshot.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_file.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"
#include "topology/arguments.hpp"
#include "topology/endpoints.hpp"

namespace satsim::topology_app {

namespace {

using app::append_csv_field;
using app::append_double;
using app::append_integer;
using app::fixed;

constexpr double kMsPerS = 1000.0;
constexpr std::size_t kMaxGatewaysListed = 5;

using app::role_name;

void append_optional_double(std::string& line, bool set, double value) {
  line.push_back(',');
  if (set) {
    append_double(line, value);
  }
}

void append_satellite(std::string& line, const constellation::Classification& c,
                      const std::optional<std::size_t>& satellite) {
  line.push_back(',');
  if (satellite) {
    append_integer(line, c.satellites[*satellite].norad_cat_id);
  }
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

std::string quantile_text(const std::vector<double>& values, int decimals, std::string_view unit) {
  const std::optional<app::Quantiles> q = app::quantiles(values);
  if (!q) {
    return "none";
  }
  const auto v = [&](double x) { return fixed(x, decimals) + std::string(unit); };
  return "min " + v(q->min) + ", median " + v(q->median) + ", p95 " + v(q->p95) + ", max " +
         v(q->max);
}

}  // namespace

PreparedRunOrError prepare_run(const Options& requested) {
  app::InputsOrError loaded = app::prepare_inputs(app::input_options_of(requested));
  if (auto* error = std::get_if<std::string>(&loaded)) {
    return std::move(*error);
  }
  PreparedRun run{std::move(std::get<app::Inputs>(loaded)), requested};
  run.options.classification = run.config;
  return run;
}

RunOutcome write_rows(const PreparedRun& run, std::ostream& csv, const RunHooks& hooks) {
  RunOutcome outcome;
  RunStatistics& stats = outcome.statistics;
  stats.directions.resize(run.directions.size());
  const Options& options = run.options;

  // The ground sites: every city, then every gateway; their visibility in one pass.
  app::StepSource steps(run, app::input_options_of(options), 1, 1, hooks.after_links);
  const std::span<const ground::GroundSite> sites = steps.sites();
  std::vector<ground::GroundSite> city_sites;
  std::vector<std::optional<ground::GroundSite>> home_pop_sites;
  for (std::size_t c = 0; c < run.cities.size(); ++c) {
    city_sites.push_back(run.cities[c].site);
    const std::optional<ground::Pop>& pop = run.home_pops[c].pop;
    home_pop_sites.push_back(pop ? std::optional(pop->site) : std::nullopt);
  }
  std::vector<Direction> directions;
  directions.reserve(run.directions.size());
  for (const PairDirection& d : run.directions) {
    directions.push_back({.from = d.from, .to = d.to});
  }
  EndpointEvaluator evaluator(std::move(city_sites), std::move(home_pop_sites), run.gateways,
                              std::move(directions), run.working.satellites.size(),
                              {.model = options.model,
                               .fibre_factor = options.fibre_factor,
                               .serving_rule = options.serving_rule});
  const bool realistic = options.model == EndpointModel::kRealistic;
  const std::string_view pattern_name = topology::name_of(options.pattern);
  const std::string_view model_name = name_of(options.model);
  const std::size_t city_count = run.cities.size();

  write_header(csv);
  std::string line;
  std::vector<PathResult> results;
  for (std::int64_t i = 0; i < run.time_count && csv; ++i) {
    const time::UtcTime utc = app::time_of_step(options.start, options.step_ns, i);
    if (auto error = steps.advance()) {  // prepare_run checked every step.
      outcome.error = std::move(*error);
      break;
    }
    const topology::StepPositions& positions = steps.positions();
    const topology::StepLinks& links = steps.links();
    const std::vector<std::vector<topology::VisibleSatellite>>& visible = steps.visible();
    app::add_checks(stats.checks,
                    topology::run_physical_checks(run.working, run.plan, links, positions,
                                                  options.limits, sites, visible, options.mask));
    stats.propagation_failures += positions.failed_count;
    stats.link_steps_up += topology::count_up(links);
    const auto down = topology::count_down_by_reason(links);
    for (std::size_t r = 0; r < down.size(); ++r) {
      stats.link_steps_down[r] += down[r];
    }
    const std::span<const std::vector<topology::VisibleSatellite>> all(visible);
    for (std::size_t s = 0; s < all.size(); ++s) {
      (s < city_count ? stats.city_link_steps : stats.gateway_link_steps) += all[s].size();
    }
    if (auto error = evaluator.evaluate(run.plan, links, all.first(city_count),
                                        all.subspan(city_count), results)) {
      outcome.error = "step " + app::format_utc_iso(utc, run.fraction_digits) + ": " + *error;
      break;
    }
    ++stats.steps;

    const std::string time_text = app::format_utc_iso(utc, run.fraction_digits);
    for (std::size_t d = 0; d < results.size(); ++d) {
      PathResult& r = results[d];
      if (hooks.before_lower_bound) {
        hooks.before_lower_bound(r);
      }
      const PairDirection& pd = run.directions[d];
      DirectionStatistics& ds = stats.directions[d];
      ++ds.steps_by_status.at(static_cast<std::size_t>(r.status));
      const bool ok = r.status == PathStatus::kOk;
      if (ok) {
        if (r.one_way_s < pd.lower_bound_s) {
          ++stats.lower_bound_failures;
          ++ds.lower_bound_failures;
        }
        ds.one_way_ms.push_back(r.one_way_s * kMsPerS);
        ds.stretch.push_back(r.one_way_s / pd.lower_bound_s);
        ++ds.laser_hop_steps[r.laser_hops];
        if (r.gateway) {
          const GatewaySite& g = run.gateways[*r.gateway];
          ++ds.gateway_steps[g.site.name + " (" + std::string(name_of(g.source)) + ")"];
        }
      }
    }
    for (std::size_t d = 0; d < results.size(); ++d) {
      const PathResult& r = results[d];
      const PairDirection& pd = run.directions[d];
      const bool ok = r.status == PathStatus::kOk;
      line.clear();
      line.append(time_text);
      line.push_back(',');
      append_csv_field(line, run.cities[pd.from].site.name);
      line.push_back(',');
      append_csv_field(line, run.cities[pd.to].site.name);
      line.push_back(',');
      line.append(role_name(pd.role));
      line.push_back(',');
      line.append(pattern_name);
      line.push_back(',');
      line.append(model_name);
      line.push_back(',');
      line.append(name_of(r.status));
      append_optional_double(line, ok, r.one_way_s * kMsPerS);
      // Directions come in pairs (from-to, to-from): the reverse of d is d ^ 1.
      const PathResult& reverse = results[d ^ 1U];
      if (!realistic) {
        append_optional_double(line, ok, 2.0 * r.one_way_s * kMsPerS);
      } else {
        append_optional_double(line, ok && reverse.status == PathStatus::kOk,
                               (r.one_way_s + reverse.one_way_s) * kMsPerS);
      }
      line.push_back(',');
      if (ok) {
        append_integer(line, static_cast<std::int64_t>(r.laser_hops));
      }
      append_optional_double(line, ok, r.path_km());
      append_optional_double(line, ok, r.uplink_km);
      append_optional_double(line, ok, r.laser_km);
      append_optional_double(line, ok, r.downlink_km);
      append_optional_double(line, ok, r.fibre_km);
      append_optional_double(line, true, pd.great_circle_km);
      append_optional_double(line, true, pd.lower_bound_s * kMsPerS);
      append_optional_double(line, ok, r.one_way_s / pd.lower_bound_s);
      append_satellite(line, run.working, r.serving_satellite);
      append_satellite(line, run.working, r.gateway_satellite);
      line.push_back(',');
      if (r.gateway) {
        append_csv_field(line, run.gateways[*r.gateway].site.name);
      }
      line.push_back(',');
      if (r.gateway) {
        line.append(name_of(run.gateways[*r.gateway].source));
      }
      line.push_back(',');
      if (realistic && run.home_pops[pd.from].pop) {
        append_csv_field(line, run.home_pops[pd.from].pop->code);
      }
      line.push_back(',');
      if (realistic) {
        line.append(name_of(run.home_pops[pd.from].level));
      }
      append_satellite(line, run.working, r.first_satellite);
      append_satellite(line, run.working, r.last_satellite);
      line.push_back('\n');
      csv.write(line.data(), static_cast<std::streamsize>(line.size()));
      ++stats.rows_written;
    }
  }
  csv.flush();
  if (!csv && !outcome.error) {
    outcome.error = "writing the CSV failed";
  }
  return outcome;
}

void write_summary(const PreparedRun& run, const RunStatistics& stats, std::ostream& out) {
  const Options& o = run.options;
  const app::InputOptions inputs = app::input_options_of(o);
  out << "topology summary\n";
  app::write_inputs_line(out, inputs, run);
  out << "  times: " << run.time_count << ", from " << run.first_time << " to " << run.last_time
      << " UTC every " << app::seconds_text(o.step_ns) << " s (steps done: " << stats.steps
      << ")\n";
  out << "  settings: pattern " << topology::name_of(o.pattern) << ", model " << name_of(o.model)
      << ", roles " << name_of(o.roles) << ", serving rule highest-elevation, "
      << app::limits_text(inputs) << '\n';
  app::write_satellite_lines(out, run);
  out << "  propagation failures at the start: " << run.start_propagation_failures
      << ", over the run: " << stats.propagation_failures << " satellite-steps\n";
  app::write_plan_lines(out, inputs, run);
  app::write_link_step_lines(out, stats.link_steps_up, stats.link_steps_down, stats.city_link_steps,
                             stats.gateway_link_steps);
  app::write_ground_lines(out, run, run.directions.size() / 2);

  out << "  paths per pair and direction:\n";
  for (std::size_t d = 0; d < run.directions.size(); ++d) {
    const PairDirection& pd = run.directions[d];
    const DirectionStatistics& ds = stats.directions.at(d);
    out << "    " << run.cities[pd.from].site.name << " -> " << run.cities[pd.to].site.name << " ("
        << role_name(pd.role) << ", " << fixed(pd.great_circle_km, 0) << " km, lower bound "
        << fixed(pd.lower_bound_s * kMsPerS, 3) << " ms):";
    for (const PathStatus status : kAllPathStatuses) {
      const std::size_t n = ds.steps_by_status.at(static_cast<std::size_t>(status));
      if (n > 0 || status == PathStatus::kOk) {
        out << ' ' << name_of(status) << ' ' << n;
      }
    }
    out << '\n';
    out << "      one-way: " << quantile_text(ds.one_way_ms, 3, " ms") << '\n';
    out << "      stretch: " << quantile_text(ds.stretch, 3, "") << '\n';
    if (!ds.laser_hop_steps.empty()) {
      out << "      laser hops (steps):";
      for (const auto& [hops, n] : ds.laser_hop_steps) {
        out << ' ' << hops << ':' << n;
      }
      out << '\n';
    }
    if (!ds.gateway_steps.empty()) {
      std::vector<std::pair<std::string, std::size_t>> used(ds.gateway_steps.begin(),
                                                            ds.gateway_steps.end());
      std::stable_sort(used.begin(), used.end(),
                       [](const auto& a, const auto& b) { return a.second > b.second; });
      out << "      gateways (" << used.size() << "):";
      for (std::size_t k = 0; k < std::min(used.size(), kMaxGatewaysListed); ++k) {
        out << (k == 0 ? " " : "; ") << used[k].first << ' ' << used[k].second;
      }
      if (used.size() > kMaxGatewaysListed) {
        out << "; ...";
      }
      out << '\n';
    }
    if (ds.lower_bound_failures > 0) {
      out << "      BELOW THE LOWER BOUND: " << ds.lower_bound_failures << " steps\n";
    }
  }

  app::write_physical_check_line(out, stats.checks);
  out << "  delays below the lower bound (must be 0): " << stats.lower_bound_failures << '\n';
  if (o.links_out) {
    out << "  laser links written to " << o.links_out->string() << '\n';
  }
  out << "  rows written: " << stats.rows_written << '\n';
  out << "  checks: " << (stats.passed() ? "PASSED" : "FAILED") << '\n';
}

int run_main(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
             const RunHooks& hooks) {
  const ParsedArguments parsed = parse_arguments(args);
  if (parsed.help) {
    out << usage();
    return 0;
  }
  if (!parsed.options) {
    err << "topology: " << parsed.error.value_or("invalid arguments") << '\n'
        << "Run 'topology --help' for usage.\n";
    return 2;
  }
  const Options& options = *parsed.options;
  PreparedRunOrError prepared = prepare_run(options);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    err << "topology: " << *error << '\n';
    return 1;
  }
  const PreparedRun& run = std::get<PreparedRun>(prepared);
  if (options.links_out) {
    if (auto error = topology::write_link_plan_file(run.plan, run.working, *options.links_out)) {
      err << "topology: --links-out " << *error << '\n';
      return 1;
    }
  }

  RunOutcome outcome;
  if (options.out == "-") {
    outcome = write_rows(run, out, hooks);
    write_summary(run, outcome.statistics, err);
  } else {
    std::ofstream file(options.out, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      err << "topology: cannot open " << options.out << " for writing\n";
      return 1;
    }
    outcome = write_rows(run, file, hooks);
    file.close();
    if (!file && !outcome.error) {
      outcome.error = "writing the CSV failed";
    }
    write_summary(run, outcome.statistics, out);
  }
  if (outcome.error) {
    err << "topology: " << *outcome.error << " (" << options.out << ")\n";
    return 1;
  }
  if (!outcome.statistics.passed()) {
    err << "topology: physical checks failed: " << outcome.statistics.checks.total()
        << " violations, " << outcome.statistics.lower_bound_failures
        << " delays below the lower bound\n";
    return 1;
  }
  return 0;
}

}  // namespace satsim::topology_app
