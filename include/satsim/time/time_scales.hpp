#pragma once

#include "satsim/time/utc_time.hpp"

namespace satsim::time {

// Instants on other time scales are carried by the same UtcTime representation (an MJD day
// plus nanoseconds of that day, with 86400 s per day). Such a value is a reading of that
// scale's clock written as a calendar date, not a UTC instant; the scale is carried by the
// name of the variable or parameter (`ut1`, `tt`). Every function in satsim that expects a
// scale other than UTC says so in its parameter name.

/// TAI - UTC in seconds: 37 s since 2017-01-01 0h UTC (IERS Bulletin C; the leap second at
/// the end of 2016-12-31 was the last one inserted). Valid until the next leap second, of
/// which none has been announced.
inline constexpr double kTaiMinusUtcS = 37.0;

/// TT - TAI in seconds, exact by definition (IAU 1991, Resolution A4).
inline constexpr double kTtMinusTaiS = 32.184;

/// The UT1 reading at a UTC instant: UTC + (UT1 - UTC), the published Earth-orientation
/// value (see EarthOrientationTable). UT1 is the Earth's rotation angle expressed as a time;
/// GMST and therefore TEME to Earth-fixed need it. |UT1 - UTC| < 0.9 s by definition of UTC.
[[nodiscard]] UtcTime ut1_from_utc(const UtcTime& utc, double ut1_minus_utc_s) noexcept;

/// The TT reading at a UTC instant: UTC + 37 s + 32.184 s = UTC + 69.184 s.
///
/// Valid assumption: TAI - UTC = 37 s, true from 2017-01-01 until the next leap second. For
/// an earlier instant the result is wrong by the leap seconds since then (e.g. 5 s too large
/// for 2004, where TAI - UTC was 32 s). TT only feeds the slow precession and nutation
/// models here: 1 s of TT shifts them by under 2e-11 rad, below 0.2 mm at a 7000 km radius.
[[nodiscard]] UtcTime tt_from_utc(const UtcTime& utc) noexcept;

}  // namespace satsim::time
