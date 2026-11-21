#include "routing/run.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "common/csv.hpp"
#include "common/statistics.hpp"
#include "common/steps.hpp"
#include "inputs/inputs.hpp"
#include "inputs/step_source.hpp"
#include "routing/arguments.hpp"
#include "routing/engine.hpp"
#include "routing/graph.hpp"
#include "routing/settings.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/routing/planner.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_file.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::routing_app {

namespace {

using app::append_csv_field;
using app::append_double;
using app::append_integer;
using app::fixed;

constexpr double kMsPerS = 1000.0;
constexpr double kSecondsPerHour = 3600.0;
constexpr double kNsPerS = 1e9;

EngineSettings settings_of(const Options& o) {
  return EngineSettings{.strategies = o.strategies,
                        .step_ns = o.step_ns,
                        .replan_ns = o.replan_ns,
                        .slot_ns = o.slot_ns,
                        .planner = o.planner,
                        .serving = o.serving,
                        .backup = o.backup,
                        .hop_delay_s = o.hop_delay_s};
}

std::optional<std::string> check_options(const Options& o) {
  if (o.strategies.empty()) {
    return std::string("no strategy");
  }
  if (o.step_ns <= 0) {
    return std::string("the step must be positive");
  }
  if (o.replan_ns <= 0 || o.replan_ns % o.step_ns != 0) {
    return std::string("the re-plan interval must be a positive whole multiple of the step");
  }
  if (o.slot_ns <= 0 || o.slot_ns % o.step_ns != 0) {
    return std::string("the serving slot must be a positive whole multiple of the step");
  }
  if (!std::isfinite(o.hop_delay_s) || o.hop_delay_s < 0.0) {
    return std::string("the hop delay must be finite and not negative");
  }
  return std::nullopt;
}

// The engine's view of the program's StepSource.
class SnapshotLookahead final : public Lookahead {
 public:
  SnapshotLookahead(const app::StepSource& steps, std::size_t city_count, double step_s)
      : steps_(&steps), city_count_(city_count), step_s_(step_s) {}

  [[nodiscard]] std::size_t known_steps() const override { return steps_->full_window(); }

  [[nodiscard]] StepGround step(std::size_t offset) const override {
    const std::span<const std::vector<topology::VisibleSatellite>> all(steps_->visible(offset));
    return StepGround{.links = &steps_->links(offset),
                      .city_visible = all.first(city_count_),
                      .gateway_visible = all.subspan(city_count_)};
  }

  [[nodiscard]] std::vector<double> times_left_in_view_s(
      std::size_t site, std::span<const topology::VisibleSatellite> visible,
      double horizon_s) const override {
    return steps_->visibility().times_left_in_view_s(site, visible, steps_->position_steps(),
                                                     step_s_, horizon_s);
  }

  [[nodiscard]] bool moving_north(std::size_t satellite) const override {
    if (steps_->position_window() < 2) {
      return false;
    }
    const auto& now = steps_->positions(0).position_ecef_km.at(satellite);
    const auto& next = steps_->positions(1).position_ecef_km.at(satellite);
    return now && next && next->z() > now->z();
  }

 private:
  const app::StepSource* steps_;
  std::size_t city_count_;
  double step_s_;
};

void write_line(std::ostream& out, std::span<const std::string_view> columns) {
  std::string line;
  for (const std::string_view column : columns) {
    if (!line.empty()) {
      line.push_back(',');
    }
    line.append(column);
  }
  line.push_back('\n');
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

void append_norad(std::string& line, const PreparedRun& run, std::optional<std::size_t> satellite) {
  line.push_back(',');
  if (satellite) {
    append_integer(line, run.working.satellites.at(*satellite).norad_cat_id);
  }
}

std::string quantile_text(const std::vector<double>& values, int decimals, std::string_view unit) {
  const std::optional<app::Quantiles> q = app::quantiles(values);
  if (!q) {
    return "none";
  }
  const auto v = [&](double x) { return fixed(x, decimals) + std::string(unit); };
  return "min " + v(q->min) + ", p5 " + v(q->p5) + ", median " + v(q->median) + ", p95 " +
         v(q->p95) + ", max " + v(q->max) + " (" + std::to_string(q->count) + ")";
}

std::string per_hour(std::size_t count, double hours) {
  return hours > 0.0 ? fixed(static_cast<double>(count) / hours, 1) : std::string("n/a");
}

std::string site_name(const PreparedRun& run, const SiteRef& site) {
  return site.gateway ? run.gateways.at(site.index).site.name : run.cities.at(site.index).site.name;
}

}  // namespace

EngineNetwork network_of(const PreparedRun& run) {
  EngineNetwork net{.model = run.options.model,
                    .satellite_count = run.working.satellites.size(),
                    .city_count = run.cities.size(),
                    .gateway_count = run.gateways.size(),
                    .plan = &run.plan,
                    .directions = {},
                    .end_delays_s = {}};
  const bool realistic = run.options.model == app::EndpointModel::kRealistic;
  const double factor = run.options.fibre_factor;
  for (const app::PairDirection& d : run.directions) {
    const auto& pop = run.home_pops[d.from].pop;
    const ground::GroundSite* pop_site = pop ? &pop->site : nullptr;
    net.directions.push_back(EngineDirection{.from = d.from,
                                             .to = d.to,
                                             .lower_bound_s = d.lower_bound_s,
                                             .has_home_pop = pop.has_value()});
    if (!realistic) {
      continue;
    }
    for (const app::GatewaySite& g : run.gateways) {
      if (pop_site == nullptr) {
        net.end_delays_s.push_back(std::nan(""));
        continue;
      }
      // As the topology program sums them: gateway to PoP, then PoP to the destination.
      const double to_pop_s = ground::fibre_delay_s(
          ground::great_circle_distance_on_mean_sphere_km(g.site, *pop_site), factor);
      const double to_destination_s = ground::fibre_delay_s(
          ground::great_circle_distance_on_mean_sphere_km(*pop_site, run.cities[d.to].site),
          factor);
      net.end_delays_s.push_back(to_pop_s + to_destination_s);
    }
  }
  return net;
}

PreparedRunOrError prepare_run(const Options& options) {
  if (auto error = check_options(options)) {
    return std::move(*error);
  }
  const EngineSettings settings = settings_of(options);
  app::InputOptions inputs = app::input_options_of(options);
  inputs.lookahead_steps =
      static_cast<std::int64_t>(position_steps_needed(settings, options.model)) - 1;
  app::InputsOrError loaded = app::prepare_inputs(inputs);
  if (auto* error = std::get_if<std::string>(&loaded)) {
    return std::move(*error);
  }
  PreparedRun run{std::move(std::get<app::Inputs>(loaded)), options};
  run.options.classification = run.config;
  if (!options.both_directions) {
    keep_forward_directions(run);
  }
  return run;
}

void keep_forward_directions(PreparedRun& run) {
  std::vector<app::PairDirection> forward;
  forward.reserve((run.directions.size() + 1) / 2);
  for (std::size_t d = 0; d < run.directions.size(); d += 2) {
    forward.push_back(run.directions[d]);
  }
  run.directions = std::move(forward);
  run.options.both_directions = false;
}

RunOutcome write_routes(const PreparedRun& run, std::ostream& csv, const RunHooks& hooks) {
  RunOutcome outcome;
  RunStatistics& stats = outcome.statistics;
  const Options& o = run.options;
  const EngineSettings settings = settings_of(o);
  const app::InputOptions inputs = app::input_options_of(o);
  Engine engine(network_of(run), settings, hooks.before_checks);
  app::StepSource steps(run, inputs, position_steps_needed(settings, o.model),
                        full_steps_needed(settings), hooks.after_links);
  const double step_s = static_cast<double>(o.step_ns) / kNsPerS;
  const SnapshotLookahead ahead(steps, run.cities.size(), step_s);
  const GraphLayout& layout = engine.layout();
  const bool realistic = o.model == app::EndpointModel::kRealistic;
  const std::size_t city_count = run.cities.size();
  std::vector<std::string> strategy_names;
  strategy_names.reserve(o.strategies.size());
  for (const routing::Strategy& s : o.strategies) {
    strategy_names.push_back(routing::name_of(s));
  }
  stats.ideal_ms.resize(run.directions.size());

  write_line(csv, kRoutesColumns);
  std::vector<RouteRow> rows;
  std::string line;
  for (std::int64_t i = 0; i < run.time_count && csv; ++i) {
    if (auto error = steps.advance()) {
      outcome.error = std::move(*error);
      break;
    }
    const topology::StepPositions& positions = steps.positions();
    const topology::StepLinks& links = steps.links();
    const std::vector<std::vector<topology::VisibleSatellite>>& visible = steps.visible();
    app::add_checks(stats.physical,
                    topology::run_physical_checks(run.working, run.plan, links, positions, o.limits,
                                                  steps.sites(), visible, o.mask));
    stats.propagation_failures += positions.failed_count;
    stats.link_steps_up += topology::count_up(links);
    const auto down = topology::count_down_by_reason(links);
    for (std::size_t r = 0; r < down.size(); ++r) {
      stats.link_steps_down.at(r) += down.at(r);
    }
    for (std::size_t s = 0; s < visible.size(); ++s) {
      (s < city_count ? stats.city_link_steps : stats.gateway_link_steps) += visible[s].size();
    }
    if (auto error = engine.step(i, ahead, rows)) {
      outcome.error =
          "step " + app::format_utc_iso(positions.utc, run.fraction_digits) + ": " + *error;
      break;
    }
    ++stats.steps;

    const std::string time_text = app::format_utc_iso(positions.utc, run.fraction_digits);
    for (const RouteRow& row : rows) {
      const app::PairDirection& pd = run.directions[row.direction];
      if (row.strategy == 0 && row.ideal_s) {
        stats.ideal_ms[row.direction].push_back(*row.ideal_s * kMsPerS);
      }
      line.clear();
      line.append(time_text);
      line.push_back(',');
      append_csv_field(line, run.cities[pd.from].site.name);
      line.push_back(',');
      append_csv_field(line, run.cities[pd.to].site.name);
      line.push_back(',');
      line.append(app::role_name(pd.role));
      line.push_back(',');
      line.append(topology::name_of(o.pattern));
      line.push_back(',');
      line.append(app::name_of(o.model));
      line.push_back(',');
      line.append(strategy_names[row.strategy]);
      line.append(row.ok ? ",ok," : ",drop,");
      if (row.cause) {
        line.append(name_of(*row.cause));
      }
      line.push_back(',');
      if (row.ok) {
        append_double(line, row.delay_s * kMsPerS);
      }
      line.push_back(',');
      if (row.ideal_s) {
        append_double(line, *row.ideal_s * kMsPerS);
      }
      // The route's satellites, laser hops and gateway.
      std::vector<std::size_t> satellites;
      std::optional<std::size_t> gateway;
      std::size_t laser_hops = 0;
      for (std::size_t n = 0; n < row.route.size(); ++n) {
        const NodeMeaning m = layout.meaning(row.route[n]);
        if (m.kind == NodeKind::kSatellite) {
          if (!satellites.empty() &&
              layout.meaning(row.route[n - 1]).kind == NodeKind::kSatellite) {
            ++laser_hops;
          }
          satellites.push_back(m.index);
        } else if (m.kind == NodeKind::kGateway) {
          gateway = m.index;
        }
      }
      line.push_back(',');
      if (row.ok) {
        append_integer(line, static_cast<std::int64_t>(laser_hops));
      }
      line.push_back(',');
      if (row.ok) {
        for (std::size_t n = 0; n < satellites.size(); ++n) {
          if (n > 0) {
            line.push_back(' ');
          }
          append_integer(line, run.working.satellites.at(satellites[n]).norad_cat_id);
        }
      }
      append_norad(
          line, run,
          row.ok && !satellites.empty() ? std::optional(satellites.front()) : std::nullopt);
      append_norad(line, run,
                   row.ok && !satellites.empty() ? std::optional(satellites.back()) : std::nullopt);
      append_norad(line, run, realistic ? row.serving : std::nullopt);
      line.push_back(',');
      if (row.ok && gateway) {
        append_csv_field(line, run.gateways.at(*gateway).site.name);
      }
      line.push_back(',');
      if (row.ok && gateway) {
        line.append(app::name_of(run.gateways.at(*gateway).source));
      }
      line.push_back(',');
      if (realistic && run.home_pops[pd.from].pop) {
        append_csv_field(line, run.home_pops[pd.from].pop->code);
      }
      line.push_back(',');
      if (row.plan) {
        line.append(routing::name_of(*row.plan));
      }
      line.push_back(',');
      if (row.backup_s) {
        append_double(line, *row.backup_s * kMsPerS);
      }
      line.push_back('\n');
      csv.write(line.data(), static_cast<std::streamsize>(line.size()));
      ++stats.rows_written;
    }
  }
  engine.finish();
  csv.flush();
  if (!csv && !outcome.error) {
    outcome.error = "writing the routes CSV failed";
  }
  stats.routes = engine.checks();
  stats.measures = engine.measures();
  stats.drop_events = engine.drop_events();
  std::sort(stats.drop_events.begin(), stats.drop_events.end(),
            [](const DropEvent& a, const DropEvent& b) {
              return std::tie(a.start_step, a.direction, a.strategy) <
                     std::tie(b.start_step, b.direction, b.strategy);
            });
  return outcome;
}

bool write_drops(const PreparedRun& run, const RunStatistics& stats, std::ostream& csv) {
  const Options& o = run.options;
  write_line(csv, kDropsColumns);
  std::string line;
  for (const DropEvent& e : stats.drop_events) {
    const app::PairDirection& pd = run.directions.at(e.direction);
    line.clear();
    line.append(app::format_utc_iso(app::time_of_step(o.start, o.step_ns, e.start_step),
                                    run.fraction_digits));
    line.push_back(',');
    line.append(app::seconds_text(e.steps * o.step_ns));
    line.push_back(',');
    append_csv_field(line, run.cities[pd.from].site.name);
    line.push_back(',');
    append_csv_field(line, run.cities[pd.to].site.name);
    line.push_back(',');
    line.append(app::role_name(pd.role));
    line.push_back(',');
    line.append(topology::name_of(o.pattern));
    line.push_back(',');
    line.append(app::name_of(o.model));
    line.push_back(',');
    line.append(routing::name_of(o.strategies.at(e.strategy)));
    line.push_back(',');
    line.append(name_of(e.cause));
    const Responsible& r = e.responsible;
    append_norad(line, run, r.satellite);
    std::optional<std::size_t> a;
    std::optional<std::size_t> b;
    if (r.link) {
      a = run.plan.links.at(*r.link).satellite_a;
      b = run.plan.links.at(*r.link).satellite_b;
    }
    append_norad(line, run, a);
    append_norad(line, run, b);
    line.push_back(',');
    if (r.link_reason) {
      line.append(topology::name_of(*r.link_reason));
    }
    line.push_back(',');
    if (r.site) {
      append_csv_field(line, site_name(run, *r.site));
    }
    line.push_back(',');
    if (r.site) {
      line.append(r.site->gateway ? "gateway" : "city");
    }
    line.push_back('\n');
    csv.write(line.data(), static_cast<std::streamsize>(line.size()));
  }
  csv.flush();
  return static_cast<bool>(csv);
}

void write_summary(const PreparedRun& run, const RunStatistics& stats, std::ostream& out) {
  const Options& o = run.options;
  const app::InputOptions inputs = app::input_options_of(o);
  const bool realistic = o.model == app::EndpointModel::kRealistic;
  out << "routing summary\n";
  app::write_inputs_line(out, inputs, run);
  out << "  times: " << run.time_count << ", from " << run.first_time << " to " << run.last_time
      << " UTC every " << app::seconds_text(o.step_ns) << " s (steps done: " << stats.steps
      << ")\n";
  out << "  planning: re-plan every " << app::seconds_text(o.replan_ns) << " s, planner "
      << name_of(o.planner) << ", ";
  if (realistic) {
    out << "serving rule " << name_of(o.serving) << ", serving slot "
        << app::seconds_text(o.slot_ns) << " s";
  } else {
    out << "no serving slot (best case: any visible satellite)";
  }
  out << ", backup " << (o.backup ? "on" : "off") << ", hop delay "
      << fixed(o.hop_delay_s * kMsPerS, 3) << " ms\n";
  out << "  strategies:";
  for (const routing::Strategy& s : o.strategies) {
    out << ' ' << routing::name_of(s);
  }
  out << '\n';
  out << "  settings: pattern " << topology::name_of(o.pattern) << ", model "
      << app::name_of(o.model) << ", roles " << app::name_of(o.roles) << ", directions "
      << (o.both_directions ? "both" : "forward") << ", " << app::limits_text(inputs) << '\n';
  app::write_satellite_lines(out, run);
  out << "  propagation failures at the start: " << run.start_propagation_failures
      << ", over the run: " << stats.propagation_failures << " satellite-steps\n";
  app::write_plan_lines(out, inputs, run);
  app::write_link_step_lines(out, stats.link_steps_up, stats.link_steps_down, stats.city_link_steps,
                             stats.gateway_link_steps);
  app::write_ground_lines(out, run,
                          o.both_directions ? run.directions.size() / 2 : run.directions.size());

  const double hours = static_cast<double>(std::max<std::int64_t>(run.time_count - 1, 0)) *
                       static_cast<double>(o.step_ns) / kNsPerS / kSecondsPerHour;
  const double step_s = static_cast<double>(o.step_ns) / kNsPerS;
  const std::size_t k_count = o.strategies.size();
  out << "  routes per pair, direction and strategy (changes per hour over " << fixed(hours, 3)
      << " h):\n";
  for (std::size_t d = 0; d < run.directions.size(); ++d) {
    const app::PairDirection& pd = run.directions[d];
    out << "    " << run.cities[pd.from].site.name << " -> " << run.cities[pd.to].site.name << " ("
        << app::role_name(pd.role) << ", " << fixed(pd.great_circle_km, 0) << " km, lower bound "
        << fixed(pd.lower_bound_s * kMsPerS, 3) << " ms)\n";
    if (d < stats.ideal_ms.size()) {
      out << "      ideal: " << quantile_text(stats.ideal_ms[d], 3, " ms") << '\n';
    }
    for (std::size_t s = 0; s < k_count && d * k_count + s < stats.measures.size(); ++s) {
      const Measures& m = stats.measures[d * k_count + s];
      out << "      " << routing::name_of(o.strategies[s]) << ": ok " << m.ok_steps << " of "
          << m.steps << " steps; plans " << m.plans << " (kept " << m.kept << ", replaced "
          << m.replaced << ", none " << m.no_route << ")\n";
      out << "        path changes per hour: "
          << per_hour(m.bad_changes + m.increase_changes, hours) << " (bad "
          << per_hour(m.bad_changes, hours) << ", delay increases "
          << per_hour(m.increase_changes, hours) << "), breaks " << per_hour(m.breaks, hours)
          << "; counts: bad " << m.bad_changes << ", increases " << m.increase_changes
          << ", breaks " << m.breaks << ", recoveries " << m.recoveries << '\n';
      out << "        change size: " << quantile_text(m.change_sizes_ms, 3, " ms") << '\n';
      out << "        route lifetime: " << quantile_text(m.lifetimes_s, 1, " s") << '\n';
      out << "        delay: " << quantile_text(m.delay_ms, 3, " ms") << '\n';
      std::vector<double> stretch;
      stretch.reserve(m.delay_ms.size());
      for (const double ms : m.delay_ms) {
        stretch.push_back(ms / (pd.lower_bound_s * kMsPerS));
      }
      out << "        stretch: " << quantile_text(stretch, 3, "") << '\n';
      out << "        extra over the ideal: " << quantile_text(m.extra_ms, 3, " ms") << '\n';
      out << "        no route:";
      bool any = false;
      for (const DropCause cause : kAllDropCauses) {
        const std::size_t n = m.drop_steps_by_cause.at(static_cast<std::size_t>(cause));
        if (n > 0) {
          out << ' ' << name_of(cause) << ' ' << fixed(static_cast<double>(n) * step_s, 1) << " s";
          any = true;
        }
      }
      out << (any ? "" : " none") << "; drop events " << m.drop_events << '\n';
      if (o.backup) {
        out << "        backup: " << quantile_text(m.backup_ms, 3, " ms") << "; extra "
            << quantile_text(m.backup_extra_ms, 3, " ms") << "; none at " << m.backup_missing
            << " plans\n";
      }
    }
  }
  const RouteChecks& c = stats.routes;
  out << "  route-check failures (must be 0): ok_route_not_in_graph " << c.ok_route_not_in_graph
      << ", below_ideal " << c.below_ideal << ", below_lower_bound " << c.below_lower_bound
      << ", drop_without_cause " << c.drop_without_cause << ", bad_route_shape "
      << c.bad_route_shape << '\n';
  app::write_physical_check_line(out, stats.physical);
  if (o.links_out) {
    out << "  laser links written to " << o.links_out->string() << '\n';
  }
  out << "  rows written: " << stats.rows_written << ", drop events: " << stats.drop_events.size()
      << '\n';
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
    err << "routing: " << parsed.error.value_or("invalid arguments") << '\n'
        << "Run 'routing --help' for usage.\n";
    return 2;
  }
  const Options& options = *parsed.options;
  PreparedRunOrError prepared = prepare_run(options);
  if (const auto* error = std::get_if<std::string>(&prepared)) {
    err << "routing: " << *error << '\n';
    return 2;
  }
  const PreparedRun& run = std::get<PreparedRun>(prepared);
  if (options.links_out) {
    if (auto error = topology::write_link_plan_file(run.plan, run.working, *options.links_out)) {
      err << "routing: --links-out " << *error << '\n';
      return 2;
    }
  }
  RunOutcome outcome;
  std::ostream* summary = &out;
  if (options.out == "-") {
    outcome = write_routes(run, out, hooks);
    summary = &err;
  } else {
    std::ofstream file(options.out, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      err << "routing: cannot open " << options.out << " for writing\n";
      return 2;
    }
    outcome = write_routes(run, file, hooks);
    file.close();
    if (!file && !outcome.error) {
      outcome.error = "writing the routes CSV failed";
    }
  }
  if (outcome.error) {
    err << "routing: " << *outcome.error << " (" << options.out << ")\n";
    return 2;
  }
  if (options.drops_out) {
    std::ofstream drops(*options.drops_out, std::ios::binary | std::ios::trunc);
    if (!drops.is_open() || !write_drops(run, outcome.statistics, drops)) {
      err << "routing: cannot write " << options.drops_out->string() << '\n';
      return 2;
    }
  }
  write_summary(run, outcome.statistics, *summary);
  if (!outcome.statistics.passed()) {
    err << "routing: checks failed: " << outcome.statistics.routes.total()
        << " route-check violations, " << outcome.statistics.physical.total()
        << " physical-check violations\n";
    return 1;
  }
  return 0;
}

}  // namespace satsim::routing_app
