// Vallado's SGP4 verification cases (SGP4-VER.TLE) against his published output (tcppver.out),
// proved twice: (a) the vendored code, as we compile it, fed the TLE values the way his TLE
// reader computes them; (b) our Propagator, built from an ElementSet. Plus checks that our
// conversions give sgp4init exactly the numbers Vallado's reader gives it, epoch included, so
// that (a) and (b) agree bit for bit.
//
// Why (a) does not call Vallado's TLE reader, twoline2rv, itself: it passes satrec.satnum to
// sgp4init as the satellite number, and sgp4init then does strcpy(satrec.satnum, satn), a copy
// of a string onto itself. Overlapping strcpy is undefined behaviour and AddressSanitizer
// stops on it (strcpy-param-overlap). Our Propagator never calls twoline2rv. ValladoRun below
// repeats twoline2rv's arithmetic (SGP4.cpp, the end of twoline2rv) with Vallado's own date
// functions instead.

#include <SGP4.h>
#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/utc_time.hpp"
#include "sgp4_inputs.hpp"
#include "sgp4_verification_data.hpp"

namespace {

using satsim::orbit::ElementSet;
using satsim::orbit::PropagationError;
using satsim::orbit::Propagator;
using satsim::orbit::TemeState;
using satsim::orbit::test::driver_times;
using satsim::orbit::test::element_set_from_tle;
using satsim::orbit::test::load_verification_cases;
using satsim::orbit::test::tle_epoch_day_of_year;
using satsim::orbit::test::tle_epoch_year;
using satsim::orbit::test::VerificationCase;

// tcppver.out prints positions with 8 decimals (km) and velocities with 9 (km/s), so a correct
// implementation is off by at most half a unit in the last place: 5e-9 km and 5e-10 km/s. The
// tolerances are 200 times that: 1 mm and 0.1 mm/s. That leaves room for last-bit differences
// between compilers, libm versions and architectures (Clang fuses a*b+c into one instruction
// on arm64; x86-64 without -march has none), which grow over long propagations, while a wrong
// constant, unit or time offset is metres to kilometres off.
constexpr double kPositionToleranceKm = 1e-6;
constexpr double kVelocityToleranceKmS = 1e-7;

// Rows are printed with 8 decimals of minutes; the driver's accumulated times are exact to
// about 1e-12 minutes.
constexpr double kTimeMatchMin = 1e-6;

constexpr std::size_t kCaseCount = 33;

// The seven cases that end in an SGP4 error, in file order, with the time of the failing call
// (the first time of the driver's schedule that has no row). python-sgp4's tests list the same
// codes: [1, 1, 6, 6, 4, 3, 6]. 33334 fails at its epoch: the driver still prints an epoch row
// for it, but with the previous satellite's values left in its buffers, so that row is not
// compared. The second 20413 case starts 1844000 minutes (3.5 years) after epoch.
struct ExpectedFailure {
  std::string_view satnum;
  double start_min;
  double minutes;
  int sgp4_code;
};
constexpr std::array<ExpectedFailure, 7> kExpectedFailures{{
    {"22312", 54.2028672, 494.2028672, 1},
    {"28350", 0.0, 1560.0, 1},
    {"28872", 0.0, 55.0, 6},
    {"29141", 0.0, 440.0, 6},
    {"33333", 0.0, 25.0, 4},
    {"33334", 0.0, 0.0, 3},
    {"20413", 1844000.0, 1844345.0, 6},
}};

const std::vector<VerificationCase>& cases() {
  static const std::vector<VerificationCase> loaded = load_verification_cases();
  return loaded;
}

std::optional<ExpectedFailure> expected_failure(const VerificationCase& c) {
  for (const ExpectedFailure& f : kExpectedFailures) {
    if (f.satnum == c.satnum && f.start_min == c.start_min) {
      return f;
    }
  }
  return std::nullopt;
}

// One evaluation: SGP4's error code (0 on success) and the state.
struct Outcome {
  int sgp4_code = 0;
  std::array<double, 3> position_km{};
  std::array<double, 3> velocity_km_s{};
};

struct Comparison {
  std::string problem;  // Empty if the schedule, the rows and the expected failure all agree.
  std::size_t rows_compared = 0;
  double max_position_error_km = 0.0;
  double max_velocity_error_km_s = 0.0;
};

// Evaluates `state_at` at every time of the driver's schedule and compares with the rows.
Comparison compare(const VerificationCase& c, const std::function<Outcome(double)>& state_at) {
  const std::optional<ExpectedFailure> failure = expected_failure(c);
  Comparison result;
  std::size_t row = 0;
  for (const double t : driver_times(c)) {
    const Outcome outcome = state_at(t);
    std::ostringstream where;
    where.precision(10);
    where << "t = " << t << " min: ";
    if (failure && std::fabs(t - failure->minutes) < kTimeMatchMin) {
      const std::size_t rows_left = c.rows.size() - row;
      const std::size_t stale_rows = failure->minutes == 0.0 ? 1 : 0;
      if (outcome.sgp4_code != failure->sgp4_code) {
        result.problem = where.str() + "SGP4 code " + std::to_string(outcome.sgp4_code) +
                         ", expected " + std::to_string(failure->sgp4_code);
      } else if (rows_left != stale_rows) {
        result.problem = where.str() + std::to_string(rows_left) + " rows left after the failure";
      }
      return result;
    }
    if (outcome.sgp4_code != 0) {
      result.problem = where.str() + "unexpected SGP4 code " + std::to_string(outcome.sgp4_code);
      return result;
    }
    if (row >= c.rows.size() || std::fabs(c.rows[row].minutes - t) >= kTimeMatchMin) {
      result.problem = where.str() + "no matching row in tcppver.out";
      return result;
    }
    const auto& expected = c.rows[row++];
    for (std::size_t k = 0; k < 3; ++k) {
      result.max_position_error_km =
          std::max(result.max_position_error_km,
                   std::fabs(outcome.position_km[k] - expected.position_km[k]));
      result.max_velocity_error_km_s =
          std::max(result.max_velocity_error_km_s,
                   std::fabs(outcome.velocity_km_s[k] - expected.velocity_km_s[k]));
    }
    ++result.rows_compared;
  }
  if (failure) {
    result.problem = "the expected SGP4 error never happened";
  } else if (row != c.rows.size()) {
    result.problem = std::to_string(c.rows.size() - row) + " rows of tcppver.out not reached";
  }
  return result;
}

void record(const Comparison& comparison) {
  std::ostringstream text;
  text.precision(3);
  text << comparison.max_position_error_km << " km, " << comparison.max_velocity_error_km_s
       << " km/s over " << comparison.rows_compared << " rows";
  testing::Test::RecordProperty("max_error", text.str());
}

// What Vallado's twoline2rv hands to sgp4init for this TLE, computed as twoline2rv does: mean
// motion and its derivatives divided by xpdotp = 1440 / (2 pi) (and by 1440 per extra
// derivative), angles in radians, and the epoch through Vallado's own days2mdhms_SGP4 and
// jday_SGP4, then (jd + jdF) - 2433281.5. Angles, eccentricity and BSTAR come from the
// ElementSet, which reads them exactly as twoline2rv does (see element_set_from_tle).
struct ValladoInputs {
  double jd = 0.0;
  double jd_fraction = 0.0;
  double epoch_days = 0.0;
  double no_kozai = 0.0;
  double ndot = 0.0;
  double nddot = 0.0;
};

ValladoInputs vallado_inputs(const VerificationCase& c, const ElementSet& e) {
  const double xpdotp = 1440.0 / (2.0 * std::numbers::pi);
  ValladoInputs in;
  const int year = tle_epoch_year(c.line1);
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  double second = 0.0;
  SGP4Funcs::days2mdhms_SGP4(year, tle_epoch_day_of_year(c.line1), month, day, hour, minute,
                             second);
  SGP4Funcs::jday_SGP4(year, month, day, hour, minute, second, in.jd, in.jd_fraction);
  in.epoch_days = (in.jd + in.jd_fraction) - 2433281.5;
  in.no_kozai = e.mean_motion_rev_per_day / xpdotp;
  in.ndot = e.mean_motion_dot_rev_per_day2 / (xpdotp * 1440.0);
  in.nddot = e.mean_motion_ddot_rev_per_day3 / (xpdotp * 1440.0 * 1440);
  return in;
}

// Vallado's code as his test driver runs it: sgp4init with the TLE's values, then sgp4 on the
// same record for each time in turn.
class ValladoRun {
 public:
  // epoch_days overrides the epoch twoline2rv would compute (to show what that changes).
  ValladoRun(const VerificationCase& c, char operation_mode,
             std::optional<double> epoch_days = std::nullopt) {
    const ElementSet e = element_set_from_tle(c.line1, c.line2);
    const ValladoInputs in = vallado_inputs(c, e);
    satrec_.jdsatepoch = in.jd;
    satrec_.jdsatepochF = in.jd_fraction;
    // The TLE's own five-character number, as twoline2rv passes it (it fits in char[6]).
    SGP4Funcs::sgp4init(wgs72, operation_mode, c.satnum.c_str(), epoch_days.value_or(in.epoch_days),
                        e.bstar_per_earth_radius, in.ndot, in.nddot, e.eccentricity,
                        e.arg_perigee_rad, e.inclination_rad, e.mean_anomaly_rad, in.no_kozai,
                        e.raan_rad, satrec_);
  }

  Outcome operator()(double minutes) {
    Outcome outcome;
    SGP4Funcs::sgp4(satrec_, minutes, outcome.position_km.data(), outcome.velocity_km_s.data());
    outcome.sgp4_code = satrec_.error;
    return outcome;
  }

 private:
  elsetrec satrec_{};
};

// Every verification element set passes our checks; a nullopt means the reader is wrong.
satsim::orbit::detail::Sgp4Inputs inputs(const ElementSet& e) {
  const auto in = satsim::orbit::detail::sgp4_inputs(e);
  if (!in.has_value()) {
    ADD_FAILURE() << "element set " << e.norad_cat_id << " rejected";
    std::abort();
  }
  return *in;
}

int sgp4_code(PropagationError error) {
  const auto code = static_cast<int>(error);
  return code >= 1 && code <= 6 ? code : -1;  // -1: one of our own errors, never expected here.
}

Outcome from_state(const satsim::orbit::StateOrError& state) {
  Outcome outcome;
  if (const auto* error = std::get_if<PropagationError>(&state)) {
    outcome.sgp4_code = sgp4_code(*error);
    return outcome;
  }
  const auto& s = std::get<TemeState>(state);
  for (std::size_t k = 0; k < 3; ++k) {
    outcome.position_km[k] = s.position_km[static_cast<Eigen::Index>(k)];
    outcome.velocity_km_s[k] = s.velocity_km_s[static_cast<Eigen::Index>(k)];
  }
  return outcome;
}

TEST(Sgp4VerificationData, FileHasTheExpectedCases) {
  ASSERT_EQ(cases().size(), kCaseCount);
  std::size_t failing = 0;
  for (const VerificationCase& c : cases()) {
    EXPECT_FALSE(c.rows.empty()) << c.satnum;
    failing += expected_failure(c).has_value() ? 1U : 0U;
  }
  EXPECT_EQ(failing, kExpectedFailures.size());
}

class Sgp4Verification : public testing::TestWithParam<std::size_t> {
 protected:
  void SetUp() override {
    ASSERT_LT(GetParam(), cases().size());
    case_ = &cases()[GetParam()];
  }
  [[nodiscard]] const VerificationCase& verification_case() const { return *case_; }

 private:
  const VerificationCase* case_ = nullptr;
};

// (a) The vendored code, compiled with our flags, reproduces tcppver.out.
TEST_P(Sgp4Verification, ValladoCodeReproducesPublishedOutput) {
  const VerificationCase& c = verification_case();
  ValladoRun run(c, 'i');
  const Comparison comparison = compare(c, std::ref(run));
  record(comparison);
  EXPECT_EQ(comparison.problem, "");
  EXPECT_LE(comparison.max_position_error_km, kPositionToleranceKm);
  EXPECT_LE(comparison.max_velocity_error_km_s, kVelocityToleranceKmS);
}

// (b) Our Propagator, built from an ElementSet read from the same TLE, reproduces tcppver.out.
// Each propagation works on a fresh copy of the record, unlike the driver's single record.
TEST_P(Sgp4Verification, PropagatorReproducesPublishedOutput) {
  const VerificationCase& c = verification_case();
  const auto created = Propagator::create(element_set_from_tle(c.line1, c.line2));
  const std::optional<ExpectedFailure> failure = expected_failure(c);
  if (failure && failure->minutes == 0.0) {
    // SGP4 cannot evaluate this orbit at its epoch, so create() refuses it with SGP4's code.
    const auto* error = std::get_if<PropagationError>(&created);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(sgp4_code(*error), failure->sgp4_code);
    return;
  }
  const auto* propagator = std::get_if<Propagator>(&created);
  ASSERT_NE(propagator, nullptr) << satsim::orbit::describe(std::get<PropagationError>(created));
  const Comparison comparison = compare(c, [propagator](double minutes) {
    return from_state(propagator->propagate_minutes_since_epoch(minutes));
  });
  record(comparison);
  EXPECT_EQ(comparison.problem, "");
  EXPECT_LE(comparison.max_position_error_km, kPositionToleranceKm);
  EXPECT_LE(comparison.max_velocity_error_km_s, kVelocityToleranceKmS);
}

// Every value our conversion hands to sgp4init equals, bit for bit, what Vallado's reader
// computes from the same TLE, including the epoch in days since 1949 December 31.
TEST_P(Sgp4Verification, ConversionMatchesValladoTleArithmetic) {
  const VerificationCase& c = verification_case();
  const ElementSet elements = element_set_from_tle(c.line1, c.line2);
  const satsim::orbit::detail::Sgp4Inputs ours = inputs(elements);
  const ValladoInputs vallado = vallado_inputs(c, elements);

  EXPECT_EQ(ours.no_kozai_rad_per_min, vallado.no_kozai);
  EXPECT_EQ(ours.ndot_rad_per_min2, vallado.ndot);
  EXPECT_EQ(ours.nddot_rad_per_min3, vallado.nddot);
  EXPECT_EQ(ours.bstar_per_earth_radius, elements.bstar_per_earth_radius);
  EXPECT_EQ(ours.eccentricity, elements.eccentricity);
  EXPECT_EQ(ours.inclination_rad, elements.inclination_rad);
  EXPECT_EQ(ours.raan_rad, elements.raan_rad);
  EXPECT_EQ(ours.arg_perigee_rad, elements.arg_perigee_rad);
  EXPECT_EQ(ours.mean_anomaly_rad, elements.mean_anomaly_rad);

  // Our fraction of day is exact to the nanosecond (1.2e-14 day); Vallado's goes from day of
  // year to hours, minutes and seconds in floating point and is off by up to about 1e-14 day.
  // Both then add the two Julian date parts, which rounds to about 5e-10 day near JD 2.45e6,
  // so the epoch sgp4init receives is the same double.
  EXPECT_EQ(ours.epoch_jd_whole, vallado.jd);
  EXPECT_NEAR(ours.epoch_jd_fraction, vallado.jd_fraction, 1e-13);
  EXPECT_EQ(ours.epoch_days_since_1949_dec_31, vallado.epoch_days);

  // The epoch survives the trip through CivilTime to the nanosecond.
  const auto epoch = satsim::time::UtcTime::from_civil(elements.epoch);
  if (epoch.has_value()) {
    EXPECT_EQ(epoch->to_civil(), elements.epoch);
  } else {
    ADD_FAILURE() << "epoch rejected";
  }
}

// Vallado's code (one record, propagated in time order) and our Propagator (a fresh copy of
// the record per call) agree bit for bit at every time: the conversions add nothing, and
// copying the record per call changes nothing, even for the resonant orbits whose integrator
// state sgp4() carries from call to call.
TEST_P(Sgp4Verification, PropagatorMatchesValladoBitForBit) {
  const VerificationCase& c = verification_case();
  const ElementSet elements = element_set_from_tle(c.line1, c.line2);
  ValladoRun vallado(c, 'i');
  const auto created = Propagator::create(elements);
  const auto* propagator = std::get_if<Propagator>(&created);
  for (const double t : driver_times(c)) {
    const Outcome expected = vallado(t);
    if (propagator == nullptr) {  // Only 33334, which fails at its epoch.
      EXPECT_EQ(sgp4_code(std::get<PropagationError>(created)), expected.sgp4_code);
      return;
    }
    const Outcome actual = from_state(propagator->propagate_minutes_since_epoch(t));
    ASSERT_EQ(actual.sgp4_code, expected.sgp4_code) << "t = " << t;
    if (actual.sgp4_code != 0) {
      return;
    }
    EXPECT_EQ(actual.position_km, expected.position_km) << "t = " << t;
    EXPECT_EQ(actual.velocity_km_s, expected.velocity_km_s) << "t = " << t;
  }
}

std::string case_name(const testing::TestParamInfo<std::size_t>& info) {
  // Index first: 20413 appears twice.
  std::string name = std::to_string(info.param);
  if (name.size() < 2) {
    name.insert(0, "0");
  }
  if (info.param < cases().size()) {
    name += "_sat" + cases()[info.param].satnum;
  }
  return name;
}

INSTANTIATE_TEST_SUITE_P(Vallado, Sgp4Verification, testing::Range(std::size_t{0}, kCaseCount),
                         case_name);

// Why sgp4_inputs rounds the epoch the way Vallado does. Deep-space SGP4 evaluates the Sun's and
// Moon's positions at the epoch it is given. Computed without that rounding, as
// (jd_whole - 2433281.5) + jd_fraction, the epoch of 23333 (WIND, eccentricity 0.97, apogee
// about 477 000 km, beyond the Moon) moves by 2.2e-10 day (19 microseconds) and its positions
// by 4.1e-6 km, four times the 1 mm tolerance. Near-Earth orbits never use the epoch value.
TEST(Sgp4EpochRounding, UnroundedEpochMovesWindByAboutFourMillimetres) {
  ASSERT_EQ(cases().size(), kCaseCount);
  const VerificationCase& wind = cases()[14];
  ASSERT_EQ(wind.satnum, "23333");
  const satsim::orbit::detail::Sgp4Inputs ours =
      inputs(element_set_from_tle(wind.line1, wind.line2));
  const double unrounded = (ours.epoch_jd_whole - 2433281.5) + ours.epoch_jd_fraction;
  EXPECT_NE(unrounded, ours.epoch_days_since_1949_dec_31);
  EXPECT_NEAR(unrounded - ours.epoch_days_since_1949_dec_31, -2.2e-10, 1e-11);

  ValladoRun run(wind, 'i', unrounded);
  const Comparison comparison = compare(wind, std::ref(run));
  EXPECT_EQ(comparison.problem, "");
  EXPECT_GT(comparison.max_position_error_km, 3e-6);
  EXPECT_LT(comparison.max_position_error_km, 5e-6);
}

// Operation mode: tcppver.out is reproduced in mode 'i' (improved), the mode Propagator uses.
// Mode 'a' (AFSPC) changes only deep-space results, and on this file only for 23599, which
// Vallado added to show the Lyddane atan2 quadrant problem: about 1 km off.
TEST(Sgp4OperationMode, AfspcModeDiffersFromPublishedOutputOnlyFor23599) {
  ASSERT_EQ(cases().size(), kCaseCount);
  for (const VerificationCase& c : cases()) {
    ValladoRun run(c, 'a');
    const Comparison comparison = compare(c, std::ref(run));
    EXPECT_EQ(comparison.problem, "") << c.satnum;
    if (c.satnum == "23599") {
      EXPECT_GT(comparison.max_position_error_km, 0.5);
    } else {
      EXPECT_LE(comparison.max_position_error_km, kPositionToleranceKm) << c.satnum;
      EXPECT_LE(comparison.max_velocity_error_km_s, kVelocityToleranceKmS) << c.satnum;
    }
  }
}

}  // namespace
