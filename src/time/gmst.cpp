#include "satsim/time/gmst.hpp"

#include <cmath>
#include <numbers>

#include "satsim/core/angles.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::time {

namespace {
constexpr double kJdJ2000 = 2451545.0;
constexpr double kDaysPerJulianCentury = 36525.0;
constexpr double kSecondsPerDay = 86400.0;
constexpr double kRadiansPerSecondOfTime = 2.0 * std::numbers::pi / kSecondsPerDay;
}  // namespace

double gmst_rad(const UtcTime& ut1) noexcept {
  // IAU 1982, in seconds of time, with T in Julian centuries of UT1 from J2000.0:
  //   GMST = 67310.54841 + (876600 h + 8640184.812866 s) T + 0.093104 T^2 - 6.2e-6 T^3
  // 876600 h * T is exactly 86400 s per elapsed day, i.e. one full turn per day, so only the
  // fractional part of the elapsed days matters for it. Evaluating it from the two-part date
  // keeps full precision instead of the ~40 us a single-double Julian date would allow.
  const double whole_days = ut1.jd_whole() - kJdJ2000;  // exact; ends in .5
  const double fraction = ut1.jd_fraction();
  const double t = (whole_days + fraction) / kDaysPerJulianCentury;

  const double day_turn_s = kSecondsPerDay * (std::fmod(whole_days, 1.0) + fraction);
  const double gmst_s =
      67310.54841 + day_turn_s + t * (8640184.812866 + t * (0.093104 + t * -6.2e-6));
  return wrap_two_pi(gmst_s * kRadiansPerSecondOfTime);
}

}  // namespace satsim::time
