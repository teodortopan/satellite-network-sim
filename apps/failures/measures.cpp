#include "failures/measures.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include "failures/loop.hpp"
#include "satsim/failure/schedule.hpp"

namespace satsim::failures_app {

namespace {

constexpr double kMsPerS = 1000.0;

using Lane = std::vector<const Interval*>;

// Calls f(from, to, a, b) for every piece of [from_s, end_s) over which the intervals of lanes
// a and b are both constant, in time order.
void walk(const Lane& a, const Lane& b, double from_s, double end_s,
          const std::function<void(double, double, const Interval&, const Interval&)>& f) {
  std::size_t i = 0;
  std::size_t j = 0;
  double t = from_s;
  while (t < end_s && i < a.size() && j < b.size()) {
    while (i < a.size() && a[i]->end_s <= t) {
      ++i;
    }
    while (j < b.size() && b[j]->end_s <= t) {
      ++j;
    }
    if (i >= a.size() || j >= b.size()) {
      break;
    }
    const double to = std::min({a[i]->end_s, b[j]->end_s, end_s});
    if (to > t) {
      f(t, to, *a[i], *b[j]);
    }
    t = to;
  }
}

}  // namespace

std::optional<double> failure_instant(const failure::FailureSchedule& schedule) {
  std::optional<double> t_f;
  for (const failure::Failure& f : schedule.failures) {
    if (f.start_s >= 0.0 && (!t_f || f.start_s < *t_f)) {
      t_f = f.start_s;
    }
  }
  return t_f;
}

std::vector<LaneMeasures> lane_measures(
    std::span<const Interval> intervals,
    const std::vector<std::vector<std::optional<double>>>& step_delays, std::size_t engines,
    std::size_t directions, std::size_t strategies, double step_s, double end_s,
    std::optional<double> t_f) {
  const std::size_t lanes = directions * strategies;
  std::vector<Lane> by_lane(engines * lanes);
  for (const Interval& iv : intervals) {
    by_lane.at(iv.engine * lanes + iv.direction * strategies + iv.strategy).push_back(&iv);
  }
  std::vector<LaneMeasures> result(engines * lanes);
  if (!t_f) {
    return result;
  }
  const double from = *t_f;
  for (std::size_t e = 0; e < engines; ++e) {
    for (std::size_t i = 0; i < lanes; ++i) {
      const Lane& lane = by_lane[e * lanes + i];
      const Lane& baseline = by_lane[i];
      const Lane& reference = by_lane[lanes + i];
      LaneMeasures& m = result[e * lanes + i];

      double black_hole = 0.0;
      double loop = 0.0;
      std::optional<double> stopped;
      std::optional<double> back;
      for (const Interval* iv : lane) {
        if (iv->end_s <= from) {
          continue;
        }
        const double start = std::max(iv->start_s, from);
        if (iv->status.state == DirectionState::kBlackHole) {
          black_hole += iv->end_s - start;
        } else if (iv->status.state == DirectionState::kLoop) {
          loop += iv->end_s - start;
        }
        const bool delivered = iv->status.state == DirectionState::kDelivered;
        if (!delivered && !stopped) {
          stopped = start;
        } else if (delivered && stopped && !back) {
          back = start;
        }
      }
      m.black_hole_s = black_hole;
      m.loop_s = loop;
      m.restore_s =
          !stopped ? std::optional(0.0) : (back ? std::optional(*back - from) : std::nullopt);

      double lost = 0.0;
      walk(baseline, lane, from, end_s,
           [&](double a, double b, const Interval& base, const Interval& mine) {
             if (base.status.state == DirectionState::kDelivered &&
                 mine.status.state != DirectionState::kDelivered) {
               lost += b - a;
             }
           });
      m.lost_s = lost;

      std::optional<double> last_differs;
      walk(reference, lane, from, end_s,
           [&](double, double b, const Interval& ref, const Interval& mine) {
             if (ref.status.state != mine.status.state || ref.status.route != mine.status.route) {
               last_differs = b;
             }
           });
      if (!last_differs) {
        m.settle_s = 0.0;
      } else if (*last_differs < end_s) {
        m.settle_s = *last_differs - from;
      }

      const std::vector<std::optional<double>>& mine = step_delays.at(e * lanes + i);
      const std::vector<std::optional<double>>& base = step_delays.at(i);
      bool settled_seen = false;
      for (std::size_t k = 0; k < mine.size() && k < base.size(); ++k) {
        const double t = static_cast<double>(k) * step_s;
        if (t < from) {
          continue;
        }
        const bool both = mine[k] && base[k];
        const double extra_ms = both ? (*mine[k] - *base[k]) * kMsPerS : 0.0;
        if (both) {
          m.extra_ms.push_back(extra_ms);
        }
        if (m.settle_s && !settled_seen && t >= from + *m.settle_s) {
          settled_seen = true;
          if (both) {
            m.extra_settled_ms = extra_ms;
          }
        }
      }
    }
  }
  return result;
}

}  // namespace satsim::failures_app
