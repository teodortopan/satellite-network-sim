#include "failures/timeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "failures/control_graph.hpp"
#include "failures/geometry.hpp"
#include "failures/view.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/many_sources.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_repair.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::failures_app {

namespace {

using failure::ElementKind;
using failure::Failure;
using topology::LaserSide;
using topology::LinkForce;
using topology::PlannedLink;

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Event kinds at one instant, in processing order.
enum class EventKind : std::uint8_t { kBegin = 0, kEnd = 1, kAcquired = 2, kMatch = 3 };

// One laser of the plan, as the pre-pass tracks it.
struct LaserState {
  LaserSide side = LaserSide::kAhead;
  /// Takes part in re-matching at all: it had a planned link or never had a partner
  /// (kNoFreePartner).
  bool usable = false;
  /// The link (index into Timeline::links) that uses it now; nullopt if it never had one.
  std::optional<std::size_t> link = std::nullopt;
  /// From when it may be re-matched: -infinity for one that never had a partner, +infinity
  /// for none yet.
  double eligible_s = kInfinity;
  /// Freed by an old failure that started at this instant (used only by the step-0 call).
  std::optional<double> old_freed_s = std::nullopt;
};

bool is_active(LinkForce force) noexcept {
  return force == LinkForce::kInForce || force == LinkForce::kAcquiring;
}

class Builder {
 public:
  Builder(const constellation::Classification& classification, const topology::LinkPlan& plan,
          const failure::FailureSchedule& schedule, const TimelineSettings& settings,
          Geometry& geometry)
      : classification_(&classification),
        plan_(&plan),
        schedule_(&schedule),
        settings_(settings),
        geometry_(&geometry),
        sat_down_(classification.satellites.size(), 0),
        gw_down_(settings.gateway_count, 0),
        link_failures_(plan.links.size(), 0) {
    tl_.links = plan.links;
    tl_.planned_count = plan.links.size();
    tl_.force.assign(plan.links.size(), std::vector<ForceChange>{
                                            {.from_s = -kInfinity, .force = LinkForce::kInForce}});
    current_.assign(plan.links.size(), LinkForce::kInForce);
    generation_.assign(plan.links.size(), 0);
    tl_.schedule = schedule;
    tl_.nodes = ControlNodes{.satellites = classification.satellites.size(),
                             .cities = geometry.at(0).city_visible.size(),
                             .gateways = settings.gateway_count};
    lasers_.resize(classification.satellites.size());
    for (std::size_t s = 0; s < plan.lasers.size() && s < lasers_.size(); ++s) {
      for (const topology::Laser& laser : plan.lasers[s]) {
        LaserState state{.side = laser.side};
        state.link = laser.link;
        if (laser.link) {
          state.usable = true;
        } else if (laser.unassigned == topology::UnassignedReason::kNoFreePartner) {
          state.usable = true;
          state.eligible_s = -kInfinity;
        }
        lasers_[s].push_back(state);
      }
    }
  }

  Timeline run() {
    process_old_failures();
    for (std::size_t i = 0; i < schedule_->failures.size(); ++i) {
      const Failure& f = schedule_->failures[i];
      const std::optional<double> end = failure::end_s(f);
      if (f.start_s >= 0.0) {
        events_.emplace(f.start_s, EventKind::kBegin, i, 0);
      }
      if (end && *end > 0.0) {
        events_.emplace(*end, EventKind::kEnd, i, 0);
      }
    }
    while (!events_.empty()) {
      const auto [t, kind, index, generation] = *events_.begin();
      events_.erase(events_.begin());
      switch (kind) {
        case EventKind::kBegin:
          begin_failure(index, t);
          break;
        case EventKind::kEnd:
          end_failure(index, t);
          break;
        case EventKind::kAcquired:
          acquired(index, generation, t);
          break;
        case EventKind::kMatch:
          match(static_cast<std::int64_t>(index), t);
          break;
      }
    }
    planner_pass();
    collect_changes();
    return std::move(tl_);
  }

 private:
  // (instant, kind, failure / link / step, generation): ordered as processed.
  using Event = std::tuple<double, EventKind, std::size_t, std::uint64_t>;

  [[nodiscard]] double step_time(std::int64_t k) const {
    return static_cast<double>(k) * settings_.step_s;
  }
  [[nodiscard]] bool in_run(std::int64_t k) const { return k >= 0 && k < settings_.step_count; }
  // The run's end, its last step's instant: nothing detected after it reports.
  [[nodiscard]] double end_s() const { return step_time(settings_.step_count - 1); }
  // The planner's knowing instant of a fact a gateway or the planner itself detects at
  // `detected_s`: never after the run's end.
  [[nodiscard]] double planner_detects(double detected_s) const {
    return detected_s > end_s() ? kInfinity : detected_s + settings_.times.planner_processing_s;
  }
  [[nodiscard]] bool best_case() const { return settings_.model == app::EndpointModel::kBestCase; }
  [[nodiscard]] bool satellite_down_at(std::size_t s, double t) const {
    return failure::satellite_down(*schedule_, s, t);
  }
  [[nodiscard]] bool gateway_down_at(std::size_t g, double t) const {
    return failure::gateway_down(*schedule_, g, t);
  }

  void set_force(std::size_t link, double t, LinkForce force) {
    std::vector<ForceChange>& changes = tl_.force[link];
    if (changes.back().from_s == t) {
      changes.back().force = force;
    } else {
      changes.push_back({.from_s = t, .force = force});
    }
    current_[link] = force;
    ++generation_[link];
  }

  std::size_t add_link(const PlannedLink& link) {
    tl_.links.push_back(link);
    tl_.force.push_back({{.from_s = -kInfinity, .force = LinkForce::kNotInForce}});
    current_.push_back(LinkForce::kNotInForce);
    generation_.push_back(0);
    return tl_.links.size() - 1;
  }

  LaserState* laser_of(std::size_t satellite, LaserSide side) {
    if (satellite >= lasers_.size()) {
      return nullptr;
    }
    for (LaserState& laser : lasers_[satellite]) {
      if (laser.side == side) {
        return &laser;
      }
    }
    return nullptr;
  }

  // Marks a laser of `link` at `satellite` eligible from `t` (if it still belongs to the link)
  // and asks for a re-matching at the first step at or after t.
  void make_eligible(std::size_t link, std::size_t satellite, LaserSide side, double t,
                     std::optional<double> old_freed_s = std::nullopt) {
    LaserState* laser = laser_of(satellite, side);
    if (laser == nullptr || laser->link != link) {
      return;
    }
    laser->eligible_s = t;
    laser->old_freed_s = old_freed_s;
    ask_match(t);
  }

  void ask_match(double t) {
    if (!settings_.times.repoint || !(t < kInfinity)) {
      return;
    }
    const std::int64_t k = first_step_at_or_after(t, settings_.step_s);
    if (in_run(k) && requested_.insert(k).second) {
      events_.emplace(step_time(k), EventKind::kMatch, static_cast<std::size_t>(k), 0);
    }
  }

  [[nodiscard]] bool laser_free(std::size_t satellite, const LaserState& laser) const {
    if (!laser.usable || sat_down_[satellite] != 0) {
      return false;
    }
    if (!laser.link) {
      return true;
    }
    const LinkForce force = current_[*laser.link];
    return force == LinkForce::kFailed || force == LinkForce::kNotInForce;
  }

  // The facts and lasers of a link that has just been failed at t_f by failure `cause`.
  void link_down_fact(std::size_t link, double t_f, std::size_t cause) {
    const PlannedLink l = tl_.links[link];
    const std::optional<double> end = failure::end_s(schedule_->failures[cause]);
    std::optional<double> seen;
    const std::int64_t k_f = step_of(t_f, settings_.step_s);
    if (in_run(k_f)) {
      if (geometry_->link_state(k_f, l.satellite_a, l.satellite_b).up()) {
        seen = t_f;
      } else {
        for (std::int64_t k = k_f + 1; in_run(k); ++k) {
          const double t_k = step_time(k);
          if (end && t_k >= *end) {
            break;
          }
          if (geometry_->link_state(k, l.satellite_a, l.satellite_b).up()) {
            seen = t_k;
            break;
          }
        }
      }
    }
    Fact fact{.subject = {.kind = SubjectKind::kLink, .index = link},
              .down = true,
              .t_s = t_f,
              .old = false,
              .detectors = {},
              .planner_known_s = kInfinity,
              .cause = cause};
    const std::array<std::pair<std::size_t, LaserSide>, 2> ends = {
        {{l.satellite_a, l.side_a}, {l.satellite_b, l.side_b}}};
    for (const auto& [satellite, side] : ends) {
      if (seen && !satellite_down_at(satellite, *seen)) {
        const double detected_s = *seen + settings_.times.detection_s;
        fact.detectors.push_back({.node = tl_.nodes.satellite(satellite), .t_s = detected_s});
        make_eligible(link, satellite, side, detected_s);
      } else if (LaserState* laser = laser_of(satellite, side);
                 laser != nullptr && laser->link == link) {
        laser->eligible_s = kInfinity;
        laser->old_freed_s.reset();
      }
    }
    add_fact(std::move(fact));
  }

  void add_fact(Fact fact) {
    std::sort(fact.detectors.begin(), fact.detectors.end(),
              [](const Detection& a, const Detection& b) { return a.node < b.node; });
    tl_.facts.push_back(std::move(fact));
  }

  // An old fact (spec 4.2): known by everyone from the first step.
  void add_old_fact(Subject subject, bool down, double t_s, std::optional<std::size_t> cause) {
    tl_.facts.push_back(Fact{.subject = subject,
                             .down = down,
                             .t_s = t_s,
                             .old = true,
                             .detectors = {},
                             .planner_known_s = 0.0,
                             .cause = cause});
  }

  // The links that `f` takes down now: in force or acquiring, and its own (a laser link) or
  // touching its satellite. In link order.
  [[nodiscard]] std::vector<std::size_t> links_going_down(const Failure& f) const {
    std::vector<std::size_t> result;
    if (f.kind == ElementKind::kLaserLink) {
      if (f.element < tl_.planned_count && is_active(current_[f.element])) {
        result.push_back(f.element);
      }
    } else if (f.kind == ElementKind::kSatellite) {
      for (std::size_t i = 0; i < tl_.links.size(); ++i) {
        const PlannedLink& l = tl_.links[i];
        if ((l.satellite_a == f.element || l.satellite_b == f.element) && is_active(current_[i])) {
          result.push_back(i);
        }
      }
    }
    return result;
  }

  // The element of `f` goes down; whether it was up before (a satellite or gateway already
  // down by another failure stays down). Links: always counted.
  bool element_goes_down(const Failure& f) {
    switch (f.kind) {
      case ElementKind::kSatellite:
        return sat_down_[f.element]++ == 0;
      case ElementKind::kGateway:
        return gw_down_[f.element]++ == 0;
      case ElementKind::kLaserLink:
        ++link_failures_[f.element];
        return true;
    }
    return false;
  }

  void process_old_failures() {
    std::vector<std::size_t> old;
    for (std::size_t i = 0; i < schedule_->failures.size(); ++i) {
      const Failure& f = schedule_->failures[i];
      const std::optional<double> end = failure::end_s(f);
      if (f.start_s < 0.0 && (!end || *end > 0.0)) {
        old.push_back(i);
      }
    }
    std::stable_sort(old.begin(), old.end(), [&](std::size_t a, std::size_t b) {
      return schedule_->failures[a].start_s < schedule_->failures[b].start_s;
    });
    for (const std::size_t i : old) {
      const Failure& f = schedule_->failures[i];
      const std::vector<std::size_t> going = links_going_down(f);
      if (!element_goes_down(f)) {
        continue;
      }
      if (f.kind == ElementKind::kSatellite) {
        add_old_fact({.kind = SubjectKind::kSatellite, .index = f.element}, true, f.start_s, i);
      } else if (f.kind == ElementKind::kGateway) {
        add_old_fact({.kind = SubjectKind::kGateway, .index = f.element}, true, f.start_s, i);
      }
      for (const std::size_t link : going) {
        set_force(link, f.start_s, LinkForce::kFailed);
        add_old_fact({.kind = SubjectKind::kLink, .index = link}, true, f.start_s, i);
        const PlannedLink& l = tl_.links[link];
        make_eligible(link, l.satellite_a, l.side_a, 0.0, f.start_s);
        make_eligible(link, l.satellite_b, l.side_b, 0.0, f.start_s);
      }
    }
  }

  // Rule (a).
  void begin_failure(std::size_t index, double t_f) {
    const Failure& f = schedule_->failures[index];
    const std::vector<std::size_t> going = links_going_down(f);
    if (!element_goes_down(f)) {
      return;
    }
    for (const std::size_t link : going) {
      set_force(link, t_f, LinkForce::kFailed);
    }
    const std::int64_t k_f = step_of(t_f, settings_.step_s);
    const double detected_s = t_f + settings_.times.detection_s;
    if (f.kind == ElementKind::kSatellite) {
      Fact fact{.subject = {.kind = SubjectKind::kSatellite, .index = f.element},
                .down = true,
                .t_s = t_f,
                .old = false,
                .detectors = ground_watchers(f.element, k_f, t_f, detected_s),
                .planner_known_s = kInfinity,
                .cause = index};
      add_fact(std::move(fact));
    } else if (f.kind == ElementKind::kGateway) {
      Fact fact{.subject = {.kind = SubjectKind::kGateway, .index = f.element},
                .down = true,
                .t_s = t_f,
                .old = false,
                .detectors = gateway_watchers(f.element, k_f, t_f, detected_s),
                .planner_known_s = planner_detects(detected_s),
                .cause = index};
      add_fact(std::move(fact));
    }
    for (const std::size_t link : going) {
      link_down_fact(link, t_f, index);
    }
  }

  // Best-case dishes, then gateways not down at t, that see `satellite` at step k, each
  // knowing at `known_s`.
  std::vector<Detection> ground_watchers(std::size_t satellite, std::int64_t k, double t,
                                         double known_s) {
    std::vector<Detection> result;
    if (!in_run(k)) {
      return result;
    }
    const StepGeometry& step = geometry_->at(k);
    if (best_case()) {
      for (std::size_t c = 0; c < step.city_visible.size() && c < tl_.nodes.cities; ++c) {
        if (routing_app::find_visible(step.city_visible[c], satellite) != nullptr) {
          result.push_back({.node = tl_.nodes.dish(c), .t_s = known_s});
        }
      }
    }
    for (std::size_t g = 0; g < step.gateway_visible.size() && g < tl_.nodes.gateways; ++g) {
      if (!gateway_down_at(g, t) &&
          routing_app::find_visible(step.gateway_visible[g], satellite) != nullptr) {
        result.push_back({.node = tl_.nodes.gateway(g), .t_s = known_s});
      }
    }
    return result;
  }

  // The satellites not down at t that gateway g sees at step k, each knowing at `known_s`.
  std::vector<Detection> gateway_watchers(std::size_t g, std::int64_t k, double t, double known_s) {
    std::vector<Detection> result;
    if (!in_run(k)) {
      return result;
    }
    const StepGeometry& step = geometry_->at(k);
    if (g >= step.gateway_visible.size()) {
      return result;
    }
    for (const topology::VisibleSatellite& v : step.gateway_visible[g]) {
      if (!satellite_down_at(v.satellite, t)) {
        result.push_back({.node = tl_.nodes.satellite(v.satellite), .t_s = known_s});
      }
    }
    return result;
  }

  // Rule (b).
  void end_failure(std::size_t index, double t_r) {
    const Failure& f = schedule_->failures[index];
    bool returned = false;
    const std::int64_t k_r = step_of(t_r, settings_.step_s);
    switch (f.kind) {
      case ElementKind::kSatellite:
        returned = --sat_down_[f.element] == 0;
        if (returned) {
          add_fact(Fact{.subject = {.kind = SubjectKind::kSatellite, .index = f.element},
                        .down = false,
                        .t_s = t_r,
                        .old = false,
                        .detectors = ground_watchers(f.element, k_r, t_r, t_r),
                        .planner_known_s = kInfinity,
                        .cause = index});
        }
        break;
      case ElementKind::kGateway:
        returned = --gw_down_[f.element] == 0;
        if (returned) {
          add_fact(Fact{.subject = {.kind = SubjectKind::kGateway, .index = f.element},
                        .down = false,
                        .t_s = t_r,
                        .old = false,
                        .detectors = gateway_watchers(f.element, k_r, t_r, t_r),
                        .planner_known_s = planner_detects(t_r),
                        .cause = index});
        }
        return;
      case ElementKind::kLaserLink:
        --link_failures_[f.element];
        returned = true;
        break;
    }
    if (!returned) {
      return;
    }
    for (const std::size_t link : failure::links_taken_down(f, *plan_)) {
      const PlannedLink l = tl_.links[link];
      if (current_[link] != LinkForce::kFailed || link_failures_[link] != 0 ||
          sat_down_[l.satellite_a] != 0 || sat_down_[l.satellite_b] != 0) {
        continue;
      }
      drop_repair_links_on(l, t_r, index);
      for (const auto& [satellite, side] :
           {std::pair{l.satellite_a, l.side_a}, std::pair{l.satellite_b, l.side_b}}) {
        if (LaserState* laser = laser_of(satellite, side)) {
          laser->link = link;
          laser->eligible_s = kInfinity;
          laser->old_freed_s.reset();
        }
      }
      set_force(link, t_r, LinkForce::kAcquiring);
      events_.emplace(t_r + settings_.times.acquisition_s, EventKind::kAcquired, link,
                      generation_[link]);
    }
    if (f.kind == ElementKind::kSatellite) {
      // A returned satellite knows at once which of its lasers are still without a link.
      for (LaserState& laser : lasers_[f.element]) {
        if (laser.link && laser_free(f.element, laser)) {
          laser.eligible_s = t_r;
          laser.old_freed_s.reset();
          ask_match(t_r);
        }
      }
    }
  }

  // Drops every repair link in force or acquiring that uses a laser of planned link `l`.
  void drop_repair_links_on(const PlannedLink& l, double t_r, std::size_t cause) {
    const auto uses = [&](std::size_t satellite, LaserSide side) {
      return (satellite == l.satellite_a && side == l.side_a) ||
             (satellite == l.satellite_b && side == l.side_b);
    };
    for (std::size_t j = tl_.planned_count; j < tl_.links.size(); ++j) {
      const PlannedLink r = tl_.links[j];
      if (!is_active(current_[j]) ||
          !(uses(r.satellite_a, r.side_a) || uses(r.satellite_b, r.side_b))) {
        continue;
      }
      set_force(j, t_r, LinkForce::kNotInForce);
      add_fact(Fact{.subject = {.kind = SubjectKind::kLink, .index = j},
                    .down = true,
                    .t_s = t_r,
                    .old = false,
                    .detectors = {{.node = tl_.nodes.satellite(r.satellite_a), .t_s = t_r},
                                  {.node = tl_.nodes.satellite(r.satellite_b), .t_s = t_r}},
                    .planner_known_s = kInfinity,
                    .cause = cause});
      // The laser at the other end is free from now (the planned link's own lasers are given
      // back to it by the caller).
      if (!uses(r.satellite_a, r.side_a)) {
        make_eligible(j, r.satellite_a, r.side_a, t_r);
      }
      if (!uses(r.satellite_b, r.side_b)) {
        make_eligible(j, r.satellite_b, r.side_b, t_r);
      }
    }
  }

  void acquired(std::size_t link, std::uint64_t generation, double t) {
    if (generation != generation_[link] || current_[link] != LinkForce::kAcquiring) {
      return;
    }
    set_force(link, t, LinkForce::kInForce);
    const PlannedLink& l = tl_.links[link];
    add_fact(Fact{.subject = {.kind = SubjectKind::kLink, .index = link},
                  .down = false,
                  .t_s = t,
                  .old = false,
                  .detectors = {{.node = tl_.nodes.satellite(l.satellite_a), .t_s = t},
                                {.node = tl_.nodes.satellite(l.satellite_b), .t_s = t}},
                  .planner_known_s = kInfinity,
                  .cause = std::nullopt});
  }

  // Rule (c), at step k (instant t = k * step_s).
  void match(std::int64_t k, double t) {
    bool newly = false;
    for (std::size_t s = 0; s < lasers_.size() && !newly; ++s) {
      for (const LaserState& laser : lasers_[s]) {
        if (laser_free(s, laser) && std::isfinite(laser.eligible_s) &&
            first_step_at_or_after(laser.eligible_s, settings_.step_s) == k) {
          newly = true;
          break;
        }
      }
    }
    if (!newly) {
      return;
    }
    const StepGeometry& step = geometry_->at(k);
    std::vector<char> down(sat_down_.size(), 0);
    for (std::size_t s = 0; s < down.size(); ++s) {
      down[s] = static_cast<char>(sat_down_[s] != 0);
    }
    std::vector<std::vector<char>> free(plan_->lasers.size());
    for (std::size_t s = 0; s < plan_->lasers.size() && s < lasers_.size(); ++s) {
      for (const LaserState& laser : lasers_[s]) {
        free[s].push_back(static_cast<char>(laser_free(s, laser) && laser.eligible_s <= t));
      }
    }
    std::vector<std::pair<std::size_t, std::size_t>> linked;
    for (std::size_t i = 0; i < tl_.links.size(); ++i) {
      if (current_[i] != LinkForce::kNotInForce) {
        linked.emplace_back(tl_.links[i].satellite_a, tl_.links[i].satellite_b);
      }
    }
    const std::vector<PlannedLink> made = topology::match_free_lasers(
        *classification_, *plan_,
        topology::RepairInput{.position_ecef_km = step.position_ecef_km,
                              .satellite_down = down,
                              .laser_free = free,
                              .linked = linked,
                              .atmosphere_km = settings_.atmosphere_km});
    for (const PlannedLink& l : made) {
      // When its acquisition starts: the match instant, or for the step-0 call the latest start
      // of the old failures that freed its lasers (a laser that never had a partner counts as
      // -infinity, one freed otherwise as the match instant).
      double start_s = -kInfinity;
      for (const auto& [satellite, side] :
           {std::pair{l.satellite_a, l.side_a}, std::pair{l.satellite_b, l.side_b}}) {
        const LaserState* laser = laser_of(satellite, side);
        double freed_s = t;
        if (laser != nullptr && k == 0 && laser->old_freed_s) {
          freed_s = *laser->old_freed_s;
        } else if (laser != nullptr && laser->eligible_s == -kInfinity) {
          freed_s = -kInfinity;
        }
        start_s = std::max(start_s, freed_s);
      }
      const std::size_t link = add_link(l);
      for (const auto& [satellite, side] :
           {std::pair{l.satellite_a, l.side_a}, std::pair{l.satellite_b, l.side_b}}) {
        if (LaserState* laser = laser_of(satellite, side)) {
          laser->link = link;
          laser->eligible_s = kInfinity;
          laser->old_freed_s.reset();
        }
      }
      if (k == 0 && start_s == -kInfinity) {
        // Matched for old failures between two lasers that never had a partner: it carries
        // traffic from the first step (spec 4.8 Details), old news.
        set_force(link, -kInfinity, LinkForce::kInForce);
        add_old_fact({.kind = SubjectKind::kLink, .index = link}, false, 0.0, std::nullopt);
        continue;
      }
      if (start_s == -kInfinity) {
        start_s = t;
      }
      const double in_force_s = start_s + settings_.times.acquisition_s;
      if (start_s < t && in_force_s <= t) {
        // Matched for old failures and acquired before the run starts: old news.
        set_force(link, in_force_s, LinkForce::kInForce);
        add_old_fact({.kind = SubjectKind::kLink, .index = link}, false, in_force_s, std::nullopt);
      } else {
        set_force(link, t, LinkForce::kAcquiring);
        events_.emplace(in_force_s, EventKind::kAcquired, link, generation_[link]);
      }
    }
  }

  // Rule (e).
  void planner_pass() {
    std::map<double, network::ManySourcePaths> paths;
    const double processing_s = settings_.times.planner_processing_s;
    for (Fact& fact : tl_.facts) {
      if (fact.old || fact.subject.kind == SubjectKind::kGateway) {
        continue;
      }
      double best = kInfinity;
      for (const Detection& d : fact.detectors) {
        double report_s = kInfinity;
        if (d.t_s > end_s()) {
          continue;  // Detected after the run's end: no report.
        }
        if (tl_.nodes.is_gateway(d.node)) {
          report_s = 0.0;
        } else {
          auto it = paths.find(d.t_s);
          if (it == paths.end()) {
            it = paths.emplace(d.t_s, report_paths(d.t_s)).first;
          }
          if (it->second.reachable(d.node)) {
            report_s = it->second.delay_s[d.node];
          }
        }
        if (report_s < kInfinity) {
          best = std::min(best, (d.t_s + report_s) + processing_s);
        }
      }
      fact.planner_known_s = best;
    }
  }

  // From every working gateway, on the control graph of the truth at t.
  network::ManySourcePaths report_paths(double t) {
    const std::int64_t k = step_of(t, settings_.step_s);
    const topology::StepLinks geometric = geometric_states(*geometry_, tl_.links, k);
    const OverlaidStep truth = overlay_truth(geometry_->at(k), geometric, tl_, t);
    const network::Graph graph =
        control_graph(tl_.nodes, tl_.links, truth.ground(), settings_.model, settings_.hop_delay_s);
    std::vector<network::Start> starts;
    for (std::size_t g = 0; g < tl_.nodes.gateways; ++g) {
      if (g >= truth.gateway_down.size() || truth.gateway_down[g] == 0) {
        starts.push_back({.node = tl_.nodes.gateway(g), .delay_s = 0.0});
      }
    }
    return network::shortest_paths_from_many(graph, starts, settings_.times.hop_processing_s);
  }

  void collect_changes() {
    std::vector<double> changes;
    for (const std::vector<ForceChange>& link : tl_.force) {
      for (const ForceChange& c : link) {
        if (c.from_s >= 0.0) {
          changes.push_back(c.from_s);
        }
      }
    }
    for (const Failure& f : schedule_->failures) {
      const std::optional<double> end = failure::end_s(f);
      if (end && *end <= 0.0) {
        continue;
      }
      if (f.start_s >= 0.0) {
        changes.push_back(f.start_s);
      }
      if (end) {
        changes.push_back(*end);
      }
    }
    std::sort(changes.begin(), changes.end());
    changes.erase(std::unique(changes.begin(), changes.end()), changes.end());
    tl_.changes = std::move(changes);
  }

  const constellation::Classification* classification_;
  const topology::LinkPlan* plan_;
  const failure::FailureSchedule* schedule_;
  TimelineSettings settings_;
  Geometry* geometry_;
  Timeline tl_;
  std::vector<std::size_t> sat_down_;
  std::vector<std::size_t> gw_down_;
  std::vector<std::size_t> link_failures_;
  std::vector<LinkForce> current_;
  std::vector<std::uint64_t> generation_;
  std::vector<std::vector<LaserState>> lasers_;
  std::set<Event> events_;
  std::set<std::int64_t> requested_;
};

}  // namespace

std::vector<topology::LinkForce> Timeline::force_at(double t) const {
  std::vector<topology::LinkForce> result;
  result.reserve(force.size());
  for (const std::vector<ForceChange>& link : force) {
    const auto after = std::upper_bound(
        link.begin(), link.end(), t, [](double v, const ForceChange& c) { return v < c.from_s; });
    result.push_back(after == link.begin() ? link.front().force : std::prev(after)->force);
  }
  return result;
}

std::vector<char> Timeline::satellite_down_at(double t) const {
  std::vector<char> down(nodes.satellites, 0);
  for (const failure::Failure& f : schedule.failures) {
    if (f.kind == failure::ElementKind::kSatellite && f.element < down.size() &&
        failure::in_force(f, t)) {
      down[f.element] = 1;
    }
  }
  return down;
}

std::vector<char> Timeline::gateway_down_at(double t) const {
  std::vector<char> down(nodes.gateways, 0);
  for (const failure::Failure& f : schedule.failures) {
    if (f.kind == failure::ElementKind::kGateway && f.element < down.size() &&
        failure::in_force(f, t)) {
      down[f.element] = 1;
    }
  }
  return down;
}

std::int64_t step_of(double t_s, double step_s) noexcept {
  const double k = std::floor(t_s / step_s);
  return k > 0.0 ? static_cast<std::int64_t>(k) : 0;
}

std::int64_t first_step_at_or_after(double t_s, double step_s) noexcept {
  if (t_s <= 0.0) {
    return 0;
  }
  auto k = static_cast<std::int64_t>(std::floor(t_s / step_s));
  // The division may round either way: settle on the product the convention uses.
  while (k > 0 && static_cast<double>(k - 1) * step_s >= t_s) {
    --k;
  }
  while (static_cast<double>(k) * step_s < t_s) {
    ++k;
  }
  return k;
}

Timeline build_timeline(const constellation::Classification& classification,
                        const topology::LinkPlan& plan, const failure::FailureSchedule& schedule,
                        const TimelineSettings& settings, Geometry& geometry) {
  return Builder(classification, plan, schedule, settings, geometry).run();
}

}  // namespace satsim::failures_app
