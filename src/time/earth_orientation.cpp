#include "satsim/time/earth_orientation.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <istream>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "satsim/time/utc_time.hpp"

namespace satsim::time {

namespace {

constexpr double kArcsecondToRad = std::numbers::pi / (180.0 * 3600.0);
constexpr double kJdOfMjdZero = 2400000.5;

// A UT1 - UTC change larger than this between consecutive days is a leap second (1 s); the
// real day-to-day change is a few milliseconds.
constexpr double kLeapSecondJumpS = 0.5;

// Plausibility limits. Polar motion has stayed within about 0.6" since records began, and
// |UT1 - UTC| < 0.9 s is how UTC is defined (before 1972 the file has no rows).
constexpr double kMaxPolarMotionArcsec = 2.0;
constexpr double kMaxUt1MinusUtcS = 1.0;

// A finals2000A field by its 1-based inclusive column range, as the readme gives it, with
// blanks removed from both ends. Columns past the end of a short line read as blank.
std::string_view field(std::string_view line, std::size_t first_column, std::size_t last_column) {
  const std::size_t begin = first_column - 1;
  if (begin >= line.size()) {
    return {};
  }
  std::string_view text = line.substr(begin, last_column - begin);
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  return text;
}

// The whole field as a finite number (locale independent), or nullopt.
std::optional<double> to_number(std::string_view text) {
  double value = 0.0;
  const char* const end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr != end || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::string quote(std::string_view text) { return "\"" + std::string(text) + "\""; }

EarthOrientationLoadResult failure(std::string message) {
  EarthOrientationLoadResult result;
  result.error = std::move(message);
  return result;
}

}  // namespace

// Has access to EarthOrientationTable's representation (declared a friend there).
struct EarthOrientationLoader {
  static EarthOrientationLoadResult load(std::istream& in) {
    if (!in) {
      return failure("input stream is not readable");
    }
    EarthOrientationTable table;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
      ++line_number;
      std::string_view view = line;
      if (!view.empty() && view.back() == '\r') {
        view.remove_suffix(1);
      }
      const std::string_view x_text = field(view, 19, 27);
      const std::string_view y_text = field(view, 38, 46);
      const std::string_view ut1_text = field(view, 59, 68);
      if (x_text.empty() || y_text.empty() || ut1_text.empty()) {
        break;  // End of the values (the file continues with dates only).
      }
      const std::string where = "line " + std::to_string(line_number) + ": ";

      const std::string_view mjd_text = field(view, 8, 15);
      const std::optional<double> mjd = to_number(mjd_text);
      if (!mjd || *mjd != std::floor(*mjd) || *mjd < 0.0 || *mjd > 1e6) {
        return failure(where + "MJD (columns 8-15) " + quote(mjd_text) +
                       " is not a whole day number");
      }
      const auto day = static_cast<std::int64_t>(*mjd);
      if (table.rows_.empty()) {
        table.first_mjd_ = day;
      } else if (day != table.last_mjd() + 1) {
        return failure(where + "MJD " + std::to_string(day) +
                       " does not follow the previous row's " + std::to_string(table.last_mjd()) +
                       " by one day");
      }

      const std::optional<double> x_arcsec = to_number(x_text);
      const std::optional<double> y_arcsec = to_number(y_text);
      const std::optional<double> ut1_minus_utc = to_number(ut1_text);
      if (!x_arcsec || std::abs(*x_arcsec) > kMaxPolarMotionArcsec) {
        return failure(where + "polar motion x (columns 19-27) " + quote(x_text) +
                       " is not a number of at most 2 arcseconds");
      }
      if (!y_arcsec || std::abs(*y_arcsec) > kMaxPolarMotionArcsec) {
        return failure(where + "polar motion y (columns 38-46) " + quote(y_text) +
                       " is not a number of at most 2 arcseconds");
      }
      if (!ut1_minus_utc || std::abs(*ut1_minus_utc) >= kMaxUt1MinusUtcS) {
        return failure(where + "UT1-UTC (columns 59-68) " + quote(ut1_text) +
                       " is not a number of less than 1 second");
      }
      table.rows_.push_back({.ut1_minus_utc_s = *ut1_minus_utc,
                             .polar_x_rad = *x_arcsec * kArcsecondToRad,
                             .polar_y_rad = *y_arcsec * kArcsecondToRad});
    }
    if (in.bad()) {
      return failure("read error after line " + std::to_string(line_number));
    }
    if (table.rows_.empty()) {
      return failure("no row with polar motion and UT1-UTC values");
    }
    EarthOrientationLoadResult result;
    result.table = std::move(table);
    return result;
  }
};

std::optional<EarthOrientation> EarthOrientationTable::at(const UtcTime& utc) const noexcept {
  // jd_whole() is an integer MJD plus 2400000.5, exact in double, so this is exact.
  const auto mjd = static_cast<std::int64_t>(utc.jd_whole() - kJdOfMjdZero);
  const double day_fraction = utc.jd_fraction();  // [0, 1)
  if (rows_.empty() || mjd < first_mjd_ || mjd > last_mjd()) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(mjd - first_mjd_);
  const EarthOrientation& before = rows_[index];
  if (day_fraction == 0.0) {
    return before;  // Exactly at a row.
  }
  if (index + 1 == rows_.size()) {
    return std::nullopt;  // After the last row.
  }
  const EarthOrientation& after = rows_[index + 1];
  if (std::abs(after.ut1_minus_utc_s - before.ut1_minus_utc_s) > kLeapSecondJumpS) {
    return std::nullopt;  // A leap second lies between the rows.
  }
  const auto lerp = [day_fraction](double a, double b) { return a + day_fraction * (b - a); };
  return EarthOrientation{
      .ut1_minus_utc_s = lerp(before.ut1_minus_utc_s, after.ut1_minus_utc_s),
      .polar_x_rad = lerp(before.polar_x_rad, after.polar_x_rad),
      .polar_y_rad = lerp(before.polar_y_rad, after.polar_y_rad),
  };
}

EarthOrientationLoadResult load_earth_orientation(std::istream& in) noexcept {
  try {
    return EarthOrientationLoader::load(in);
  } catch (const std::exception& e) {
    return failure(std::string("could not load Earth orientation data: ") + e.what());
  }
}

EarthOrientationLoadResult load_earth_orientation(const std::filesystem::path& path) noexcept {
  try {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
      return failure(path.string() + " is a directory");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return failure("cannot open " + path.string());
    }
    EarthOrientationLoadResult result = load_earth_orientation(file);
    if (result.error) {
      result.error = path.string() + ": " + *result.error;
    }
    return result;
  } catch (const std::exception& e) {
    return failure(path.string() + ": could not load Earth orientation data: " + e.what());
  }
}

}  // namespace satsim::time
