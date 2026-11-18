#include "sgp4_verification_data.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "satsim/core/angles.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::orbit::test {

namespace {

constexpr const char* kTlePath = "tests/orbit/data/SGP4-VER.TLE";
constexpr const char* kExpectedPath = "tests/orbit/data/tcppver.out";

// Line 2 of a TLE is 69 characters; the verification file appends start, stop and step.
constexpr std::size_t kTleLineLength = 69;

std::string_view trim(std::string_view text) {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  return text;
}

// Columns [begin, end), 0-based, of a line, trimmed of spaces.
std::string_view field(const std::string& line, std::size_t begin, std::size_t end) {
  if (end > line.size()) {
    ADD_FAILURE() << "line too short for columns " << begin << "-" << end << ": " << line;
    return {};
  }
  return trim(std::string_view(line).substr(begin, end - begin));
}

double to_double(std::string_view text) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  double value = std::numeric_limits<double>::quiet_NaN();
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    ADD_FAILURE() << "not a number: \"" << text << "\"";
    return std::numeric_limits<double>::quiet_NaN();
  }
  return value;
}

std::int64_t to_integer(std::string_view text) {
  std::int64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    ADD_FAILURE() << "not an integer: \"" << text << "\"";
  }
  return value;
}

// A TLE "exponential" field of 8 columns starting at `begin`: sign (' ', '+' or '-'), five
// mantissa digits with an implied leading decimal point, exponent sign, exponent digit. For
// example " 28098-4" is 0.28098e-4. Computed as twoline2rv does: mantissa * pow(10, exponent).
double tle_exponential(const std::string& line, std::size_t begin) {
  const std::string_view text = std::string_view(line).substr(begin, 8);
  const double mantissa = to_double("0." + std::string(text.substr(1, 5)));
  const auto exponent = static_cast<int>(to_integer(text.substr(7, 1)));
  const int exponent_sign = text[6] == '-' ? -1 : 1;
  const double signed_mantissa = text[0] == '-' ? -mantissa : mantissa;
  return signed_mantissa * std::pow(10.0, exponent_sign * exponent);
}

// Appends the first seven numbers of a tcppver.out row. False if they cannot be read.
bool parse_row(const std::string& line, std::vector<ExpectedRow>& rows) {
  std::istringstream in(line);
  ExpectedRow row;
  in >> row.minutes >> row.position_km[0] >> row.position_km[1] >> row.position_km[2] >>
      row.velocity_km_s[0] >> row.velocity_km_s[1] >> row.velocity_km_s[2];
  if (!in) {
    return false;
  }
  rows.push_back(row);
  return true;
}

}  // namespace

std::vector<VerificationCase> load_verification_cases() {
  std::ifstream tle(kTlePath);
  std::ifstream expected(kExpectedPath);
  if (!tle.is_open() || !expected.is_open()) {
    ADD_FAILURE() << "cannot open " << kTlePath << " or " << kExpectedPath;
    return {};
  }

  std::vector<VerificationCase> cases;
  std::string line;
  while (std::getline(tle, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.starts_with("1 ")) {
      cases.emplace_back();
      cases.back().line1 = line;
      cases.back().satnum = std::string(field(line, 2, 7));
    } else if (line.starts_with("2 ")) {
      if (cases.empty() || !cases.back().line2.empty() || line.size() <= kTleLineLength) {
        ADD_FAILURE() << "unexpected line 2: " << line;
        return {};
      }
      VerificationCase& c = cases.back();
      c.line2 = line;
      std::istringstream extras(line.substr(kTleLineLength));
      extras >> c.start_min >> c.stop_min >> c.step_min;
      if (!extras) {
        ADD_FAILURE() << "no start, stop and step on: " << line;
        return {};
      }
    }
  }

  // tcppver.out: a header line "<catalogue number> xx" per satellite, then its rows.
  std::size_t block = 0;
  bool in_block = false;
  while (std::getline(expected, line)) {
    if (line.find("xx") != std::string::npos) {
      if (in_block) {
        ++block;
      }
      in_block = true;
      if (block >= cases.size() ||
          to_integer(trim(line.substr(0, line.find(' ')))) != to_integer(cases[block].satnum)) {
        ADD_FAILURE() << "tcppver.out block " << block << " (" << line
                      << ") does not match the TLE file";
        return {};
      }
      continue;
    }
    if (!in_block || !parse_row(line, cases[block].rows)) {
      ADD_FAILURE() << "unreadable tcppver.out line: " << line;
      return {};
    }
  }
  if (block + 1 != cases.size()) {
    ADD_FAILURE() << "tcppver.out has " << block + 1 << " blocks for " << cases.size() << " TLEs";
    return {};
  }
  return cases;
}

std::vector<double> driver_times(const VerificationCase& c) {
  // testcpp.cpp prints the state at epoch first, then runs this loop.
  std::vector<double> times{0.0};
  double t = c.start_min;
  if (std::fabs(t) > 1.0e-8) {
    t -= c.step_min;  // So the loop's first increment lands on start.
  }
  while (t < c.stop_min) {
    t += c.step_min;
    if (t > c.stop_min) {
      t = c.stop_min;
    }
    times.push_back(t);
  }
  return times;
}

int tle_epoch_year(const std::string& line1) {
  // YY at columns 19-20 (1-based). Years 57-99 are 19xx.
  const auto two_digit_year = static_cast<int>(to_integer(field(line1, 18, 20)));
  return two_digit_year < 57 ? 2000 + two_digit_year : 1900 + two_digit_year;
}

double tle_epoch_day_of_year(const std::string& line1) { return to_double(field(line1, 20, 32)); }

ElementSet element_set_from_tle(const std::string& line1, const std::string& line2) {
  ElementSet e;
  e.norad_cat_id = to_integer(field(line1, 2, 7));

  // Epoch: DDD.DDDDDDDD at columns 21-32 (1-based), read as integers.
  const int year = tle_epoch_year(line1);
  const std::int64_t day_of_year = to_integer(field(line1, 20, 23));
  const std::int64_t fraction_1e8 = to_integer(field(line1, 24, 32));
  const auto start_of_year = time::UtcTime::from_civil(time::CivilTime{year, 1, 1, 0, 0, 0.0});
  if (!start_of_year || line1[23] != '.') {
    ADD_FAILURE() << "bad epoch on: " << line1;
    return e;
  }
  constexpr std::int64_t kNanosecondsPer1e8Day = 864'000;  // 86400 s * 1e-8 * 1e9.
  const time::UtcTime epoch =
      start_of_year->plus_seconds(static_cast<double>((day_of_year - 1) * 86'400))
          .plus_seconds(static_cast<double>(fraction_1e8 * kNanosecondsPer1e8Day) * 1e-9);
  e.epoch = epoch.to_civil();

  e.mean_motion_dot_rev_per_day2 = to_double(field(line1, 33, 43));
  e.mean_motion_ddot_rev_per_day3 = tle_exponential(line1, 44);
  e.bstar_per_earth_radius = tle_exponential(line1, 53);

  e.inclination_rad = deg_to_rad(to_double(field(line2, 8, 16)));
  e.raan_rad = deg_to_rad(to_double(field(line2, 17, 25)));
  e.eccentricity = to_double("0." + std::string(field(line2, 26, 33)));
  e.arg_perigee_rad = deg_to_rad(to_double(field(line2, 34, 42)));
  e.mean_anomaly_rad = deg_to_rad(to_double(field(line2, 43, 51)));
  e.mean_motion_rev_per_day = to_double(field(line2, 52, 63));
  return e;
}

}  // namespace satsim::orbit::test
