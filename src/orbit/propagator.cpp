#include "satsim/orbit/propagator.hpp"

#include <SGP4.h>

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "sgp4_inputs.hpp"

namespace satsim::orbit {

// Vallado's record, after sgp4init. Never modified afterwards: each propagation copies it.
struct Propagator::Record {
  elsetrec satrec{};  // Value-initialised: every field, including ones sgp4init skips, is 0.
};

namespace {

// Fixed settings, documented on Propagator.
constexpr gravconsttype kGravityModel = wgs72;
constexpr char kOperationMode = 'i';

// sgp4init copies the satellite number with strcpy into elsetrec::satnum, a char[6]. Our
// catalogue numbers have up to nine digits, so the real one would overflow it. This
// placeholder (one character and the terminator) is passed instead; the identity stays in the
// ElementSet, and SGP4 never uses the number in its computation.
constexpr const char* kSatnumPlaceholder = "0";

PropagationError from_sgp4_code(int code) noexcept {
  switch (code) {
    case 1:
      return PropagationError::kMeanEccentricityOutOfRange;
    case 2:
      return PropagationError::kMeanMotionNotPositive;
    case 3:
      return PropagationError::kPerturbedEccentricityOutOfRange;
    case 4:
      return PropagationError::kSemiLatusRectumNegative;
    case 5:
      return PropagationError::kSubOrbitalAtEpoch;
    case 6:
      return PropagationError::kDecayed;
    default:
      return PropagationError::kUnknownSgp4Error;
  }
}

bool all_finite(const std::array<double, 3>& v) noexcept {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// Runs SGP4 on a copy of `initialised`, which is left untouched.
StateOrError run_sgp4(const elsetrec& initialised, double minutes_since_epoch) noexcept {
  if (!std::isfinite(minutes_since_epoch)) {
    // An infinite time would never leave the resonance integrator's stepping loop.
    return PropagationError::kNonFiniteTime;
  }
  elsetrec satrec = initialised;
  std::array<double, 3> r_km{};
  std::array<double, 3> v_km_s{};
  const bool ok = SGP4Funcs::sgp4(satrec, minutes_since_epoch, r_km.data(), v_km_s.data());
  if (!ok || satrec.error != 0) {
    return from_sgp4_code(satrec.error);
  }
  if (!all_finite(r_km) || !all_finite(v_km_s)) {
    return PropagationError::kNonFiniteResult;
  }
  return TemeState{Eigen::Vector3d(r_km[0], r_km[1], r_km[2]),
                   Eigen::Vector3d(v_km_s[0], v_km_s[1], v_km_s[2])};
}

}  // namespace

std::string_view describe(PropagationError error) noexcept {
  switch (error) {
    case PropagationError::kMeanEccentricityOutOfRange:
      return "SGP4 error 1: mean eccentricity out of range (>= 1 or < -0.001)";
    case PropagationError::kMeanMotionNotPositive:
      return "SGP4 error 2: mean motion not positive";
    case PropagationError::kPerturbedEccentricityOutOfRange:
      return "SGP4 error 3: perturbed eccentricity outside [0, 1]";
    case PropagationError::kSemiLatusRectumNegative:
      return "SGP4 error 4: semi-latus rectum negative";
    case PropagationError::kSubOrbitalAtEpoch:
      return "SGP4 error 5: elements sub-orbital at epoch";
    case PropagationError::kDecayed:
      return "SGP4 error 6: satellite has decayed";
    case PropagationError::kUnknownSgp4Error:
      return "SGP4 returned an unknown error code";
    case PropagationError::kInvalidElements:
      return "invalid element set";
    case PropagationError::kNonFiniteTime:
      return "time is not finite";
    case PropagationError::kNonFiniteResult:
      return "SGP4 returned a non-finite position or velocity";
  }
  return "unknown propagation error";
}

Propagator::Propagator(std::shared_ptr<const Record> record, const time::UtcTime& epoch) noexcept
    : record_(std::move(record)), epoch_(epoch) {}

PropagatorOrError Propagator::create(const ElementSet& elements) {
  const std::optional<detail::Sgp4Inputs> in = detail::sgp4_inputs(elements);
  const std::optional<time::UtcTime> epoch = time::UtcTime::from_civil(elements.epoch);
  if (!in || !epoch) {  // sgp4_inputs already rejects a bad epoch; both checks keep it local.
    return PropagationError::kInvalidElements;
  }

  auto record = std::make_shared<Record>();
  elsetrec& satrec = record->satrec;
  // sgp4init does not set these; twoline2rv does. Kept consistent with the epoch passed below.
  satrec.jdsatepoch = in->epoch_jd_whole;
  satrec.jdsatepochF = in->epoch_jd_fraction;
  SGP4Funcs::sgp4init(kGravityModel, kOperationMode, kSatnumPlaceholder,
                      in->epoch_days_since_1949_dec_31, in->bstar_per_earth_radius,
                      in->ndot_rad_per_min2, in->nddot_rad_per_min3, in->eccentricity,
                      in->arg_perigee_rad, in->inclination_rad, in->mean_anomaly_rad,
                      in->no_kozai_rad_per_min, in->raan_rad, satrec);
  // sgp4init always returns true; it reports a failure of its own propagation to the epoch
  // in satrec.error.
  if (satrec.error != 0) {
    return from_sgp4_code(satrec.error);
  }
  // A propagator whose epoch state is unusable is refused rather than handed out.
  const StateOrError at_epoch = run_sgp4(satrec, 0.0);
  if (const auto* error = std::get_if<PropagationError>(&at_epoch)) {
    return *error;
  }
  return Propagator(std::move(record), *epoch);
}

StateOrError Propagator::propagate(const time::UtcTime& utc) const {
  return propagate_minutes_since_epoch(utc.seconds_since(epoch_) / 60.0);
}

StateOrError Propagator::propagate_minutes_since_epoch(double minutes) const {
  return run_sgp4(record_->satrec, minutes);
}

}  // namespace satsim::orbit
