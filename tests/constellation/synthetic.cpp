#include "synthetic.hpp"

#include <SGP4.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <vector>

#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "sgp4_inputs.hpp"

namespace satsim::constellation::testing {

namespace {

constexpr double kWgs72MuKm3PerS2 = 398600.8;
constexpr double kWgs72EquatorialRadiusKm = 6378.135;
constexpr double kSecondsPerDay = 86400.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

}  // namespace

std::int64_t walker_id(const WalkerShell& shell, std::size_t p, std::size_t s) {
  return shell.first_id + static_cast<std::int64_t>(p * shell.per_plane + s);
}

std::vector<orbit::ElementSet> walker_element_sets(const WalkerShell& shell) {
  const double a_km = kWgs72EquatorialRadiusKm + shell.height_km;
  const double mean_motion_rad_s = std::sqrt(kWgs72MuKm3PerS2 / (a_km * a_km * a_km));
  const auto total = static_cast<double>(shell.planes * shell.per_plane);
  std::vector<orbit::ElementSet> sets;
  for (std::size_t p = 0; p < shell.planes; ++p) {
    for (std::size_t s = 0; s < shell.per_plane; ++s) {
      orbit::ElementSet e;
      e.object_name = "WALKER-" + std::to_string(p) + "-" + std::to_string(s);
      e.object_id = shell.object_id;
      e.norad_cat_id = walker_id(shell, p, s);
      e.epoch = shell.epoch;
      e.mean_motion_rev_per_day = mean_motion_rad_s * kSecondsPerDay / kTwoPi;
      e.eccentricity = 1e-4;
      e.inclination_rad = deg_to_rad(shell.inclination_deg);
      e.raan_rad = wrap_two_pi(deg_to_rad(shell.first_raan_deg) +
                               kTwoPi * static_cast<double>(p) / static_cast<double>(shell.planes));
      e.arg_perigee_rad = 0.0;
      e.mean_anomaly_rad =
          wrap_two_pi(kTwoPi * static_cast<double>(s) / static_cast<double>(shell.per_plane) +
                      kTwoPi * static_cast<double>(shell.phasing) * static_cast<double>(p) / total);
      e.bstar_per_earth_radius = 0.0;
      sets.push_back(e);
    }
  }
  return sets;
}

orbit::ElementSet at_epoch(const orbit::ElementSet& elements, const time::CivilTime& new_epoch) {
  const std::optional<orbit::detail::Sgp4Inputs> in = orbit::detail::sgp4_inputs(elements);
  if (!in || elements.bstar_per_earth_radius != 0.0) {
    ADD_FAILURE() << "at_epoch needs valid elements without drag";
    std::abort();
  }
  elsetrec satrec{};
  SGP4Funcs::sgp4init(wgs72, 'i', "0", in->epoch_days_since_1949_dec_31, 0.0, 0.0, 0.0,
                      in->eccentricity, in->arg_perigee_rad, in->inclination_rad,
                      in->mean_anomaly_rad, in->no_kozai_rad_per_min, in->raan_rad, satrec);
  const double minutes = utc(new_epoch).seconds_since(utc(elements.epoch)) / 60.0;
  orbit::ElementSet shifted = elements;
  shifted.epoch = new_epoch;
  shifted.mean_anomaly_rad = wrap_two_pi(elements.mean_anomaly_rad + satrec.mdot * minutes);
  shifted.arg_perigee_rad = wrap_two_pi(elements.arg_perigee_rad + satrec.argpdot * minutes);
  shifted.raan_rad = wrap_two_pi(elements.raan_rad + satrec.nodedot * minutes);
  return shifted;
}

time::UtcTime utc(const time::CivilTime& civil) {
  const std::optional<time::UtcTime> t = time::UtcTime::from_civil(civil);
  if (!t) {
    ADD_FAILURE() << "bad test time";
    std::abort();
  }
  return *t;
}

}  // namespace satsim::constellation::testing
