#include "satsim/orbit/propagator.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <set>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "sgp4_inputs.hpp"

namespace {

using satsim::deg_to_rad;
using satsim::orbit::ElementSet;
using satsim::orbit::PropagationError;
using satsim::orbit::Propagator;
using satsim::orbit::StateOrError;
using satsim::orbit::TemeState;
using satsim::orbit::detail::sgp4_inputs;
using satsim::time::CivilTime;
using satsim::time::UtcTime;

// STARLINK-1008 from tests/orbit/data/omm_fixture.json (standard GP data).
ElementSet starlink_1008() {
  ElementSet e;
  e.object_name = "STARLINK-1008";
  e.norad_cat_id = 44714;
  e.epoch = CivilTime{2026, 11, 16, 23, 6, 34.909056};
  e.mean_motion_rev_per_day = 15.70448124;
  e.eccentricity = 0.00027374;
  e.inclination_rad = deg_to_rad(53.1456);
  e.raan_rad = deg_to_rad(44.5949);
  e.arg_perigee_rad = deg_to_rad(7.7459);
  e.mean_anomaly_rad = deg_to_rad(352.3601);
  e.bstar_per_earth_radius = 0.0014401987;
  e.mean_motion_dot_rev_per_day2 = 0.00183364;
  return e;
}

// A Molniya-like orbit: 2 revolutions per day, so SGP4 uses its deep-space branch with the
// 12-hour resonance terms and their numerical integrator (the state sgp4() mutates).
ElementSet molniya() {
  ElementSet e;
  e.norad_cat_id = 8195;
  e.epoch = CivilTime{2006, 6, 25, 7, 58, 18.143616};
  e.mean_motion_rev_per_day = 2.00491383;
  e.eccentricity = 0.6877146;
  e.inclination_rad = deg_to_rad(64.1586);
  e.raan_rad = deg_to_rad(279.0717);
  e.arg_perigee_rad = deg_to_rad(264.7651);
  e.mean_anomaly_rad = deg_to_rad(20.2257);
  e.bstar_per_earth_radius = 0.00011873;
  return e;
}

Propagator make(const ElementSet& e) {
  auto created = Propagator::create(e);
  const auto* error = std::get_if<PropagationError>(&created);
  EXPECT_EQ(error, nullptr) << (error != nullptr ? satsim::orbit::describe(*error) : "");
  // A failed create ends the test at the std::get below with an exception, after the message.
  return std::get<Propagator>(std::move(created));
}

TemeState state(const StateOrError& result) {
  const auto* error = std::get_if<PropagationError>(&result);
  EXPECT_EQ(error, nullptr) << (error != nullptr ? satsim::orbit::describe(*error) : "");
  return std::get<TemeState>(result);
}

void expect_identical(const TemeState& a, const TemeState& b) {
  for (Eigen::Index k = 0; k < 3; ++k) {
    EXPECT_EQ(a.position_km[k], b.position_km[k]) << k;
    EXPECT_EQ(a.velocity_km_s[k], b.velocity_km_s[k]) << k;
  }
}

// The fixtures below are valid by construction; a nullopt means the test itself is wrong.
satsim::orbit::detail::Sgp4Inputs inputs(const ElementSet& e) {
  const auto in = sgp4_inputs(e);
  if (!in.has_value()) {
    ADD_FAILURE() << "fixture element set rejected";
    std::abort();
  }
  return *in;
}

UtcTime utc(const CivilTime& civil) {
  const auto t = UtcTime::from_civil(civil);
  if (!t.has_value()) {
    ADD_FAILURE() << "invalid fixture date";
    std::abort();
  }
  return *t;
}

PropagationError create_error(const ElementSet& e) {
  const auto created = Propagator::create(e);
  const auto* error = std::get_if<PropagationError>(&created);
  EXPECT_NE(error, nullptr);
  return error != nullptr ? *error : PropagationError::kUnknownSgp4Error;
}

// ---- Unit conversions, traced by hand ----

TEST(Sgp4InputsTest, EpochIsDaysSince1949December31) {
  ElementSet e = starlink_1008();
  e.epoch = CivilTime{1950, 1, 1, 0, 0, 0.0};
  auto in = inputs(e);
  EXPECT_EQ(in.epoch_days_since_1949_dec_31, 1.0);
  EXPECT_EQ(in.epoch_jd_whole, 2433282.5);
  EXPECT_EQ(in.epoch_jd_fraction, 0.0);

  // J2000.0, 2000-01-01 12:00 UTC, is JD 2451545.0: 2451545.0 - 2433281.5 = 18263.5 days.
  e.epoch = CivilTime{2000, 1, 1, 12, 0, 0.0};
  in = inputs(e);
  EXPECT_EQ(in.epoch_days_since_1949_dec_31, 18263.5);
  EXPECT_EQ(in.epoch_jd_whole, 2451544.5);
  EXPECT_EQ(in.epoch_jd_fraction, 0.5);
}

TEST(Sgp4InputsTest, MeanMotionAndDerivativesInRadiansAndMinutes) {
  ElementSet e = starlink_1008();
  e.mean_motion_rev_per_day = 1.0;
  e.mean_motion_dot_rev_per_day2 = 1.0;
  e.mean_motion_ddot_rev_per_day3 = 1.0;
  const auto in = inputs(e);
  // One revolution per day is 2*pi radians per 1440 minutes.
  constexpr double kTwoPi = 2.0 * std::numbers::pi;
  EXPECT_DOUBLE_EQ(in.no_kozai_rad_per_min, kTwoPi / 1440.0);
  EXPECT_DOUBLE_EQ(in.ndot_rad_per_min2, kTwoPi / (1440.0 * 1440.0));
  EXPECT_DOUBLE_EQ(in.nddot_rad_per_min3, kTwoPi / (1440.0 * 1440.0 * 1440.0));

  // STARLINK-1008: 15.70448124 rev/day = 0.0685237262... rad/min (computed independently as
  // 15.70448124 * 2 * pi / 1440).
  EXPECT_NEAR(inputs(starlink_1008()).no_kozai_rad_per_min, 0.0685237262, 1e-10);
}

TEST(Sgp4InputsTest, AnglesBstarAndEccentricityPassThrough) {
  const ElementSet e = starlink_1008();
  const auto in = inputs(e);
  EXPECT_EQ(in.inclination_rad, e.inclination_rad);
  EXPECT_EQ(in.raan_rad, e.raan_rad);
  EXPECT_EQ(in.arg_perigee_rad, e.arg_perigee_rad);
  EXPECT_EQ(in.mean_anomaly_rad, e.mean_anomaly_rad);
  EXPECT_EQ(in.bstar_per_earth_radius, e.bstar_per_earth_radius);
  EXPECT_EQ(in.eccentricity, e.eccentricity);
}

// ---- create() ----

TEST(PropagatorTest, CreateRejectsInvalidElementSets) {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  const auto with = [](auto change) {
    ElementSet e = starlink_1008();
    change(e);
    return e;
  };
  const std::vector<ElementSet> invalid = {
      with([](ElementSet& e) { e.mean_motion_rev_per_day = 0.0; }),
      with([](ElementSet& e) { e.mean_motion_rev_per_day = -15.0; }),
      with([&](ElementSet& e) { e.mean_motion_rev_per_day = kNan; }),
      with([&](ElementSet& e) { e.mean_motion_rev_per_day = kInf; }),
      with([](ElementSet& e) { e.eccentricity = 1.0; }),
      with([](ElementSet& e) { e.eccentricity = -1e-9; }),
      with([&](ElementSet& e) { e.eccentricity = kNan; }),
      with([](ElementSet& e) { e.inclination_rad = -1e-9; }),
      with([](ElementSet& e) { e.inclination_rad = std::numbers::pi + 1e-9; }),
      with([&](ElementSet& e) { e.raan_rad = kNan; }),
      with([&](ElementSet& e) { e.arg_perigee_rad = kInf; }),
      with([&](ElementSet& e) { e.mean_anomaly_rad = -kInf; }),
      with([&](ElementSet& e) { e.bstar_per_earth_radius = kNan; }),
      with([&](ElementSet& e) { e.mean_motion_dot_rev_per_day2 = kNan; }),
      with([&](ElementSet& e) { e.mean_motion_ddot_rev_per_day3 = kInf; }),
      with([](ElementSet& e) { e.epoch = CivilTime{2026, 2, 29, 0, 0, 0.0}; }),  // Not a leap year.
      with([](ElementSet& e) { e.epoch = CivilTime{2026, 11, 16, 23, 59, 60.0}; }),
      with([](ElementSet& e) { e.epoch = CivilTime{}; }),
  };
  for (std::size_t i = 0; i < invalid.size(); ++i) {
    EXPECT_FALSE(sgp4_inputs(invalid[i]).has_value()) << "case " << i;
    EXPECT_EQ(create_error(invalid[i]), PropagationError::kInvalidElements) << "case " << i;
  }
}

TEST(PropagatorTest, LimitsOfTheValidRangeAreAccepted) {
  ElementSet e = starlink_1008();
  e.eccentricity = 0.0;  // SGP4 clamps it to 1e-6 internally.
  e.inclination_rad = 0.0;
  EXPECT_TRUE(std::holds_alternative<Propagator>(Propagator::create(e)));
  e.inclination_rad = std::numbers::pi;
  EXPECT_TRUE(std::holds_alternative<Propagator>(Propagator::create(e)));
}

// Elements that pass our checks but that SGP4 cannot evaluate even at their epoch: create()
// returns SGP4's code rather than a propagator that can never give a state. 0.00001 rev/day
// is Vallado's verification case 33334, which ends in code 3.
TEST(PropagatorTest, CreateReturnsSgp4ErrorAtEpoch) {
  ElementSet e = molniya();
  e.eccentricity = 0.5602877;
  e.mean_motion_rev_per_day = 0.00001;
  EXPECT_EQ(create_error(e), PropagationError::kPerturbedEccentricityOutOfRange);
}

// elsetrec::satnum is char[6]; a nine-digit catalogue number must never reach it. Under ASan
// an overflow here would abort the test. The number also must not change the result.
TEST(PropagatorTest, NineDigitCatalogNumber) {
  ElementSet small = starlink_1008();
  small.norad_cat_id = 1;
  ElementSet large = small;
  large.norad_cat_id = 999'999'999;
  ElementSet real = small;
  real.norad_cat_id = 799'501'782;  // STARLINK-40096 in the fixture.

  const Propagator p_small = make(small);
  const Propagator p_large = make(large);
  const Propagator p_real = make(real);
  for (const double minutes : {0.0, 90.0, -1440.0}) {
    const TemeState s = state(p_small.propagate_minutes_since_epoch(minutes));
    expect_identical(state(p_large.propagate_minutes_since_epoch(minutes)), s);
    expect_identical(state(p_real.propagate_minutes_since_epoch(minutes)), s);
  }
}

// ---- propagate() ----

TEST(PropagatorTest, EpochIsTheElementSetEpoch) {
  const ElementSet e = starlink_1008();
  const Propagator p = make(e);
  EXPECT_EQ(p.epoch(), utc(e.epoch));
}

TEST(PropagatorTest, UtcAndMinutesSinceEpochAgree) {
  const Propagator p = make(starlink_1008());
  expect_identical(state(p.propagate(p.epoch())), state(p.propagate_minutes_since_epoch(0.0)));
  expect_identical(state(p.propagate(p.epoch().plus_seconds(3600.0))),
                   state(p.propagate_minutes_since_epoch(60.0)));
  expect_identical(state(p.propagate(p.epoch().plus_seconds(-86400.0))),
                   state(p.propagate_minutes_since_epoch(-1440.0)));
  // An instant given as a calendar time: 2026-11-17 00:00:00 is 53 min 25.090944 s after the
  // epoch 2026-11-16 23:06:34.909056.
  const UtcTime midnight = utc(CivilTime{2026, 11, 17, 0, 0, 0.0});
  const TemeState at_midnight = state(p.propagate(midnight));
  const TemeState by_minutes = state(p.propagate_minutes_since_epoch(3205.090944 / 60.0));
  EXPECT_LT((at_midnight.position_km - by_minutes.position_km).norm(), 1e-9);
}

TEST(PropagatorTest, NonFiniteTimeIsAnError) {
  const Propagator p = make(molniya());
  for (const double minutes :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    const StateOrError result = p.propagate_minutes_since_epoch(minutes);
    ASSERT_TRUE(std::holds_alternative<PropagationError>(result));
    EXPECT_EQ(std::get<PropagationError>(result), PropagationError::kNonFiniteTime);
  }
}

// A decaying satellite: the error comes back as a value, never as a position.
TEST(PropagatorTest, DecayIsAnErrorValue) {
  // Vallado's case 28872 (MINOTAUR R/B), which decays 55 minutes after epoch.
  ElementSet e;
  e.norad_cat_id = 28872;
  e.epoch = CivilTime{2005, 11, 29, 0, 28, 58.939104};
  e.mean_motion_rev_per_day = 16.46015938;
  e.eccentricity = 0.0303955;
  e.inclination_rad = deg_to_rad(96.4736);
  e.raan_rad = deg_to_rad(157.9986);
  e.arg_perigee_rad = deg_to_rad(244.0492);
  e.mean_anomaly_rad = deg_to_rad(110.6523);
  e.bstar_per_earth_radius = 0.00024476;
  const Propagator p = make(e);
  EXPECT_TRUE(std::holds_alternative<TemeState>(p.propagate_minutes_since_epoch(50.0)));
  const StateOrError result = p.propagate_minutes_since_epoch(55.0);
  ASSERT_TRUE(std::holds_alternative<PropagationError>(result));
  EXPECT_EQ(std::get<PropagationError>(result), PropagationError::kDecayed);
}

// Results must not depend on call order, and copies of a Propagator are interchangeable. The
// orbit is a resonant one, whose integrator state sgp4() carries in the record between calls.
// (Vallado's integrator restarts at epoch or continues from a multiple of its 720-minute step,
// so even a shared record would give these numbers; the copy per call is what keeps
// concurrent calls free of data races, which only ThreadSanitizer could prove.)
TEST(PropagatorTest, ResultsDoNotDependOnCallHistory) {
  const Propagator p = make(molniya());
  const TemeState first = state(p.propagate_minutes_since_epoch(2880.0));
  (void)p.propagate_minutes_since_epoch(-4000.0);
  (void)p.propagate_minutes_since_epoch(10000.0);
  expect_identical(state(p.propagate_minutes_since_epoch(2880.0)), first);

  const Propagator copy = p;  // NOLINT(performance-unnecessary-copy-initialization)
  expect_identical(state(copy.propagate_minutes_since_epoch(2880.0)), first);
  const Propagator fresh = make(molniya());
  expect_identical(state(fresh.propagate_minutes_since_epoch(2880.0)), first);
}

// Several threads on one Propagator get exactly the sequential results. Without
// ThreadSanitizer this cannot show the absence of a data race, only that it has no visible effect
// here.
TEST(PropagatorTest, ConcurrentCallsOnOnePropagatorAgreeWithSequentialOnes) {
  const Propagator p = make(molniya());
  constexpr int kTimes = 40;
  const auto minutes_at = [](int i) { return -3000.0 + 250.0 * i; };
  std::vector<TemeState> sequential;
  sequential.reserve(kTimes);
  for (int i = 0; i < kTimes; ++i) {
    sequential.push_back(state(p.propagate_minutes_since_epoch(minutes_at(i))));
  }

  constexpr std::size_t kThreads = 4;
  std::vector<std::vector<TemeState>> results(kThreads);
  {
    std::vector<std::jthread> threads;
    for (std::size_t t = 0; t < kThreads; ++t) {
      threads.emplace_back([&p, &results, &minutes_at, t] {
        // Each thread walks the times in a different order.
        for (int j = 0; j < kTimes; ++j) {
          const int i = static_cast<int>((static_cast<std::size_t>(j) * (2 * t + 1)) % kTimes);
          const auto result = p.propagate_minutes_since_epoch(minutes_at(i));
          results[t].push_back(std::get<TemeState>(result));
        }
      });
    }
  }
  for (std::size_t t = 0; t < kThreads; ++t) {
    ASSERT_EQ(results[t].size(), static_cast<std::size_t>(kTimes));
    for (int j = 0; j < kTimes; ++j) {
      const int i = static_cast<int>((static_cast<std::size_t>(j) * (2 * t + 1)) % kTimes);
      expect_identical(results[t][static_cast<std::size_t>(j)],
                       sequential[static_cast<std::size_t>(i)]);
    }
  }
}

TEST(PropagatorTest, EveryErrorHasADistinctDescription) {
  const std::vector<PropagationError> all = {
      PropagationError::kMeanEccentricityOutOfRange,
      PropagationError::kMeanMotionNotPositive,
      PropagationError::kPerturbedEccentricityOutOfRange,
      PropagationError::kSemiLatusRectumNegative,
      PropagationError::kSubOrbitalAtEpoch,
      PropagationError::kDecayed,
      PropagationError::kUnknownSgp4Error,
      PropagationError::kInvalidElements,
      PropagationError::kNonFiniteTime,
      PropagationError::kNonFiniteResult,
  };
  std::set<std::string_view> seen;
  for (const PropagationError error : all) {
    const std::string_view text = satsim::orbit::describe(error);
    EXPECT_FALSE(text.empty());
    EXPECT_TRUE(seen.insert(text).second) << text;
  }
  // SGP4's codes keep their numbers.
  EXPECT_EQ(static_cast<int>(PropagationError::kMeanEccentricityOutOfRange), 1);
  EXPECT_EQ(static_cast<int>(PropagationError::kDecayed), 6);
}

}  // namespace
