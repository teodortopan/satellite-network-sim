#include "satsim/time/civil_time.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>

namespace satsim::time {

namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool all_digits(std::string_view text) noexcept {
  return !text.empty() && std::all_of(text.begin(), text.end(), is_digit);
}

// Caller guarantees all_digits(text) and at most 4 characters, so this cannot overflow.
int to_int(std::string_view text) noexcept {
  int value = 0;
  for (const char c : text) {
    value = value * 10 + (c - '0');
  }
  return value;
}

}  // namespace

std::optional<CivilTime> parse_iso_civil_time(std::string_view text) noexcept {
  constexpr std::size_t kBaseLength = 19;  // "YYYY-MM-DDTHH:MM:SS"
  constexpr std::size_t kMaxFractionDigits = 9;

  if (text.size() < kBaseLength) {
    return std::nullopt;
  }
  const auto field = [text](std::size_t pos, std::size_t len) { return text.substr(pos, len); };
  if (!all_digits(field(0, 4)) || text[4] != '-' || !all_digits(field(5, 2)) || text[7] != '-' ||
      !all_digits(field(8, 2)) || text[10] != 'T' || !all_digits(field(11, 2)) || text[13] != ':' ||
      !all_digits(field(14, 2)) || text[16] != ':' || !all_digits(field(17, 2))) {
    return std::nullopt;
  }
  if (text.size() > kBaseLength) {
    const std::string_view fraction = text.substr(kBaseLength + 1);
    if (text[kBaseLength] != '.' || !all_digits(fraction) || fraction.size() > kMaxFractionDigits) {
      return std::nullopt;
    }
  }

  CivilTime civil;
  civil.year = to_int(field(0, 4));
  civil.month = to_int(field(5, 2));
  civil.day = to_int(field(8, 2));
  civil.hour = to_int(field(11, 2));
  civil.minute = to_int(field(14, 2));
  // "SS" or "SS.fff", already checked to be digits and one dot, so from_chars reads all of it.
  const std::string_view seconds = text.substr(17);
  const auto [end, ec] = std::from_chars(seconds.data(), seconds.data() + seconds.size(),
                                         civil.second, std::chars_format::fixed);
  if (ec != std::errc{} || end != seconds.data() + seconds.size()) {
    return std::nullopt;
  }
  return civil;
}

}  // namespace satsim::time
