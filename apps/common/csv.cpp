#include "common/csv.hpp"

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::app {

namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr int kMaxFractionDigits = 9;

// Nanoseconds into the minute of `utc`, exactly. to_civil's second is the integer nanosecond
// count divided by 1e9 (below 60, so within 1e-14 s of it): rounding back recovers the count.
std::int64_t nanoseconds_of_minute(const time::UtcTime& utc) noexcept {
  return std::llround(utc.to_civil().second * static_cast<double>(kNanosecondsPerSecond));
}

// Appends `value` (>= 0) in decimal, padded with leading zeros to at least `width` digits.
void append_padded(std::string& out, std::int64_t value, int width) {
  std::array<char, 24> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  assert(ec == std::errc{});
  const auto digits = static_cast<int>(end - buffer.data());
  if (digits < width) {
    out.append(static_cast<std::size_t>(width - digits), '0');
  }
  out.append(buffer.data(), end);
}

}  // namespace

void append_double(std::string& out, double value) {
  // 24 characters hold any shortest double, e.g. "-2.2250738585072014e-308".
  std::array<char, 32> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  assert(ec == std::errc{});
  out.append(buffer.data(), end);
}

void append_integer(std::string& out, std::int64_t value) {
  std::array<char, 24> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  assert(ec == std::errc{});
  out.append(buffer.data(), end);
}

void append_csv_field(std::string& out, std::string_view text) {
  if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
    out.append(text);
    return;
  }
  out.push_back('"');
  for (const char c : text) {
    if (c == '"') {
      out.push_back('"');
    }
    out.push_back(c);
  }
  out.push_back('"');
}

int fraction_digits_for(const time::UtcTime& start, std::int64_t step_ns) noexcept {
  const std::int64_t start_ns = nanoseconds_of_minute(start) % kNanosecondsPerSecond;
  std::int64_t unit_ns = kNanosecondsPerSecond;  // Unit of the last digit written.
  int digits = 0;
  while (digits < kMaxFractionDigits && (start_ns % unit_ns != 0 || step_ns % unit_ns != 0)) {
    unit_ns /= 10;
    ++digits;
  }
  return digits;
}

std::string format_utc_iso(const time::UtcTime& utc, int fraction_digits) {
  assert(fraction_digits >= 0 && fraction_digits <= kMaxFractionDigits);
  const time::CivilTime civil = utc.to_civil();
  const std::int64_t ns = nanoseconds_of_minute(utc);
  std::string text;
  text.reserve(30);
  append_padded(text, civil.year, 4);
  text.push_back('-');
  append_padded(text, civil.month, 2);
  text.push_back('-');
  append_padded(text, civil.day, 2);
  text.push_back('T');
  append_padded(text, civil.hour, 2);
  text.push_back(':');
  append_padded(text, civil.minute, 2);
  text.push_back(':');
  append_padded(text, ns / kNanosecondsPerSecond, 2);
  if (fraction_digits > 0) {
    std::int64_t fraction = ns % kNanosecondsPerSecond;
    for (int i = fraction_digits; i < kMaxFractionDigits; ++i) {
      fraction /= 10;
    }
    text.push_back('.');
    append_padded(text, fraction, fraction_digits);
  }
  return text;
}

std::string fixed(double value, int decimals) {
  std::array<char, 64> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                       std::chars_format::fixed, decimals);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("?");
}

std::string seconds_text(std::int64_t ns) {
  std::string text;
  append_integer(text, ns / kNanosecondsPerSecond);
  const std::int64_t fraction = ns % kNanosecondsPerSecond;
  if (fraction != 0) {
    // Nine digits, then trailing zeros dropped.
    std::string digits = std::to_string(fraction);
    digits.insert(0, 9 - digits.size(), '0');
    digits.erase(digits.find_last_not_of('0') + 1);
    text += "." + digits;
  }
  return text;
}

}  // namespace satsim::app
