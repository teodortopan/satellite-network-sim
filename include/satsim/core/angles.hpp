#pragma once

namespace satsim {

/// Converts an angle from degrees to radians. Any finite input; the result is not wrapped.
[[nodiscard]] double deg_to_rad(double degrees) noexcept;

/// Converts an angle from radians to degrees. Any finite input; the result is not wrapped.
[[nodiscard]] double rad_to_deg(double radians) noexcept;

/// Wraps a finite angle in radians to the half-open range [0, 2*pi).
/// The result is always strictly less than 2*pi, including for tiny negative inputs
/// whose naive wrap would round up to exactly 2*pi. Behaviour for NaN or infinity is
/// unspecified.
[[nodiscard]] double wrap_two_pi(double radians) noexcept;

}  // namespace satsim
