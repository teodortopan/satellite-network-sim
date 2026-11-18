#pragma once

#include <Eigen/Core>
#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>

#include "satsim/orbit/element_set.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::orbit {

/// Position and velocity in TEME (True Equator, Mean Equinox), the inertial frame SGP4
/// outputs. Rotate with satsim::frames::teme_to_ecef before comparing with places on Earth.
struct TemeState {
  Eigen::Vector3d position_km;
  Eigen::Vector3d velocity_km_s;
};

/// Why an element set could not be propagated. Values 1 to 6 are SGP4's own error codes
/// (Vallado's elsetrec::error), with the same numbers.
enum class PropagationError : std::uint8_t {
  /// SGP4 code 1: after the secular drag and gravity terms, the mean eccentricity is >= 1 or
  /// < -0.001. Typical of a satellite that has decayed or of nonsense elements.
  kMeanEccentricityOutOfRange = 1,
  /// SGP4 code 2: the mean motion became zero or negative (deep-space resonance terms).
  kMeanMotionNotPositive = 2,
  /// SGP4 code 3: after the lunar-solar periodics, the eccentricity left [0, 1] (deep space).
  kPerturbedEccentricityOutOfRange = 3,
  /// SGP4 code 4: the semi-latus rectum is negative, so the orbit has no real geometry.
  kSemiLatusRectumNegative = 4,
  /// SGP4 code 5: the elements are sub-orbital at epoch. The vendored SGP4 version no longer
  /// raises it (decay is detected by code 6 instead); kept so every code has a value.
  kSubOrbitalAtEpoch = 5,
  /// SGP4 code 6: the computed radius is below one Earth radius: the satellite has decayed.
  kDecayed = 6,
  /// SGP4 reported an error code outside 1 to 6. Should not happen with the vendored version.
  kUnknownSgp4Error,
  /// The element set was rejected before SGP4 ran: a non-finite value, mean motion not
  /// positive, eccentricity outside [0, 1), inclination outside [0, pi], or an epoch that is
  /// not a valid UTC date and time.
  kInvalidElements,
  /// The requested time is NaN or infinite.
  kNonFiniteTime,
  /// SGP4 reported success but the position or velocity is not finite. Never returned as a
  /// state.
  kNonFiniteResult,
};

/// A short English description of the error, for messages and logs.
[[nodiscard]] std::string_view describe(PropagationError error) noexcept;

class Propagator;

/// The result of building a propagator: one or the other, never both.
using PropagatorOrError = std::variant<Propagator, PropagationError>;

/// The result of one propagation: a state, or why there is none. A failed propagation is never
/// a position.
using StateOrError = std::variant<TemeState, PropagationError>;

/// SGP4 for one element set: from mean elements to a TEME position and velocity at any time.
///
/// Wraps David Vallado's reference implementation ("SGP4 Version 2020-07-13", vendored
/// unmodified under third_party/sgp4), with two fixed settings:
/// - Gravity constants WGS-72. GP element sets are fitted with them, so they are the right
///   ones to read them back, even though latitude and longitude later use WGS-84.
/// - Operation mode 'i' ("improved"), the mode that reproduces Vallado's published test output. The
///   other mode, 'a' (AFSPC), differs only in deep-space cases (orbital period of 225 minutes
///   or more); for Starlink-like orbits the two give identical results.
///
/// Thread safety: every member function is const and touches no shared mutable state. SGP4
/// writes into the record it propagates (the time, its error code, and for deep-space
/// resonant orbits the state of its numerical integrator), so each propagation works on its
/// own copy of the record made from the one built at construction. One Propagator can
/// therefore be used from several threads at once. Copies share the immutable record and are
/// cheap. The cost is a copy of about 1 kB per call and, for 12 h and 24 h resonant
/// orbits, re-integrating the resonance terms from epoch on every call; neither matters for
/// Starlink-like orbits, which have no resonance terms.
///
/// A moved-from Propagator may only be destroyed or assigned to.
class Propagator {
 public:
  /// Initialises SGP4 (Vallado's sgp4init) for `elements`. Returns kInvalidElements if the
  /// element set is rejected before SGP4 runs (see PropagationError), or SGP4's own error
  /// if it cannot evaluate the orbit even at its epoch. The catalogue number is not passed
  /// to SGP4 (it plays no part in the computation). The epoch is handed to sgp4init exactly
  /// as Vallado's TLE reader computes it, rounding included (about 40 microseconds; it only
  /// affects deep-space orbits), so results match the reference implementation bit for bit.
  /// epoch() and propagate() still use the exact UtcTime epoch for time since epoch.
  [[nodiscard]] static PropagatorOrError create(const ElementSet& elements);

  /// State at a UTC instant. Time since epoch is computed from the two instants with
  /// UtcTime::seconds_since, so no precision is lost to a single-double Julian date.
  [[nodiscard]] StateOrError propagate(const time::UtcTime& utc) const;

  /// State `minutes` after the epoch (negative: before), the time argument SGP4 itself takes.
  /// A NaN or infinite `minutes` gives kNonFiniteTime.
  [[nodiscard]] StateOrError propagate_minutes_since_epoch(double minutes) const;

  /// The element set's epoch, as a UTC instant.
  [[nodiscard]] const time::UtcTime& epoch() const noexcept { return epoch_; }

 private:
  struct Record;  // Vallado's elsetrec, kept out of this header.

  Propagator(std::shared_ptr<const Record> record, const time::UtcTime& epoch) noexcept;

  std::shared_ptr<const Record> record_;  // Immutable after create(); never null unless moved.
  time::UtcTime epoch_;
};

}  // namespace satsim::orbit
