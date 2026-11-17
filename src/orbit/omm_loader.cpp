#include "satsim/orbit/omm_loader.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <fstream>
#include <istream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>
#include <system_error>
#include <utility>

#include "satsim/core/angles.hpp"

namespace satsim::orbit {

namespace {

using nlohmann::json;

constexpr std::int64_t kMaxCatalogNumber = 999'999'999;  // OMM allows nine digits.
constexpr std::size_t kMaxQuotedLength = 40;             // Keeps messages short on junk input.

// Shortest round-trip text for a number, independent of the C locale.
std::string format_number(double value) {
  std::array<char, 32> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("?");
}

std::string format_number(std::uint64_t value) {
  std::array<char, 24> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("?");
}

std::string format_number(std::int64_t value) {
  std::array<char, 24> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("?");
}

std::string quote(std::string_view text) {
  if (text.size() > kMaxQuotedLength) {
    return "\"" + std::string(text.substr(0, kMaxQuotedLength)) + "...\"";
  }
  return "\"" + std::string(text) + "\"";
}

// "a number" or "a string" style phrase for a JSON value's type, for messages.
std::string describe_type(const json& value) {
  const std::string_view name = value.type_name();
  if (name == "null") {
    return "null";
  }
  const bool vowel = name.front() == 'a' || name.front() == 'o';
  return std::string(vowel ? "an " : "a ") + std::string(name);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool all_digits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), is_digit);
}

// Caller guarantees all_digits(text) and at most 4 characters, so this cannot overflow.
int to_int(std::string_view text) {
  int value = 0;
  for (const char c : text) {
    value = value * 10 + (c - '0');
  }
  return value;
}

// Parses EPOCH strictly as YYYY-MM-DDTHH:MM:SS with an optional fraction of 1 to 9 digits.
// CelesTrak sends exactly six fraction digits and no zone designator; the time is UTC.
// Only field ranges are checked here: whether the date exists is the time module's job.
// Returns an error message, or nothing on success.
std::optional<std::string> parse_epoch(std::string_view text, time::CivilTime& out) {
  constexpr std::size_t kBaseLength = 19;  // "YYYY-MM-DDTHH:MM:SS"
  constexpr std::size_t kMaxFractionDigits = 9;
  std::string bad_format = "EPOCH " + quote(text) + " is not YYYY-MM-DDTHH:MM:SS[.ffffff]";

  if (text.size() < kBaseLength) {
    return bad_format;
  }
  const auto field = [text](std::size_t pos, std::size_t len) { return text.substr(pos, len); };
  if (!all_digits(field(0, 4)) || text[4] != '-' || !all_digits(field(5, 2)) || text[7] != '-' ||
      !all_digits(field(8, 2)) || text[10] != 'T' || !all_digits(field(11, 2)) || text[13] != ':' ||
      !all_digits(field(14, 2)) || text[16] != ':' || !all_digits(field(17, 2))) {
    return bad_format;
  }
  if (text.size() > kBaseLength) {
    const std::string_view fraction = text.substr(kBaseLength + 1);
    if (text[kBaseLength] != '.' || !all_digits(fraction) || fraction.size() > kMaxFractionDigits) {
      return bad_format;
    }
  }

  time::CivilTime epoch;
  epoch.year = to_int(field(0, 4));
  epoch.month = to_int(field(5, 2));
  epoch.day = to_int(field(8, 2));
  epoch.hour = to_int(field(11, 2));
  epoch.minute = to_int(field(14, 2));
  // "SS" or "SS.fff", already checked to be digits and one dot, so from_chars reads all of it.
  const std::string_view seconds = text.substr(17);
  const auto [end, ec] = std::from_chars(seconds.data(), seconds.data() + seconds.size(),
                                         epoch.second, std::chars_format::fixed);
  if (ec != std::errc{} || end != seconds.data() + seconds.size()) {
    return bad_format;
  }

  const std::string prefix = "EPOCH " + quote(text) + ": ";
  if (epoch.month < 1 || epoch.month > 12) {
    return prefix + "month " + std::to_string(epoch.month) + " is outside 1-12";
  }
  if (epoch.day < 1 || epoch.day > 31) {
    return prefix + "day " + std::to_string(epoch.day) + " is outside 1-31";
  }
  if (epoch.hour > 23) {
    return prefix + "hour " + std::to_string(epoch.hour) + " is outside 0-23";
  }
  if (epoch.minute > 59) {
    return prefix + "minute " + std::to_string(epoch.minute) + " is outside 0-59";
  }
  // Two digits allow 60 to 99. 60 would be a leap second, which GP epochs never use.
  if (!(epoch.second < 60.0)) {
    return prefix + "second " + format_number(epoch.second) + " is outside [0, 60)";
  }
  out = epoch;
  return std::nullopt;
}

enum class Need : std::uint8_t { kRequired, kOptional };

// Validates one record and collects every problem found, so a user fixing a file sees all of
// them at once.
class RecordReader {
 public:
  explicit RecordReader(const json& record) : record_(&record) {}

  [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }

  // Finds key; records a problem and returns nullptr if it is missing (when required).
  const json* find(const char* key, Need need) {
    const auto it = record_->find(key);
    if (it == record_->end()) {
      if (need == Need::kRequired) {
        problems_.push_back(std::string(key) + " missing");
      }
      return nullptr;
    }
    return &*it;
  }

  // A finite JSON number (integer or not: CelesTrak writes 53.0 as 53). Strings are not
  // accepted because CelesTrak never sends numbers as strings.
  std::optional<double> number(const char* key, Need need) {
    const json* value = find(key, need);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (!value->is_number()) {
      problems_.push_back(std::string(key) + " is " + describe_type(*value) +
                          ", expected a number");
      return std::nullopt;
    }
    const auto result = value->get<double>();
    if (!std::isfinite(result)) {
      // JSON has no inf or NaN literal and nlohmann rejects overflow (1e400) while parsing,
      // so this is unreachable today; it keeps the contract independent of the parser.
      problems_.push_back(std::string(key) + " is not finite");
      return std::nullopt;
    }
    return result;
  }

  std::optional<std::string> string(const char* key, Need need) {
    const json* value = find(key, need);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (!value->is_string()) {
      problems_.push_back(std::string(key) + " is " + describe_type(*value) +
                          ", expected a string");
      return std::nullopt;
    }
    return value->get<std::string>();
  }

  // A JSON integer in [min, max]. A value written with a fraction or exponent is rejected.
  std::optional<std::int64_t> integer(const char* key, Need need, std::int64_t min,
                                      std::int64_t max) {
    const json* value = find(key, need);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (!value->is_number_integer()) {
      problems_.push_back(std::string(key) + " is " + describe_type(*value) +
                          ", expected an integer");
      return std::nullopt;
    }
    // nlohmann stores non-negative integers as unsigned and negative ones as signed.
    if (value->is_number_unsigned()) {
      const auto unsigned_value = value->get<std::uint64_t>();
      if (unsigned_value <= static_cast<std::uint64_t>(max) &&
          static_cast<std::int64_t>(unsigned_value) >= min) {
        return static_cast<std::int64_t>(unsigned_value);
      }
      problems_.push_back(std::string(key) + " = " + format_number(unsigned_value) +
                          " is outside [" + format_number(min) + ", " + format_number(max) + "]");
      return std::nullopt;
    }
    const auto signed_value = value->get<std::int64_t>();
    if (signed_value >= min && signed_value <= max) {
      return signed_value;
    }
    problems_.push_back(std::string(key) + " = " + format_number(signed_value) + " is outside [" +
                        format_number(min) + ", " + format_number(max) + "]");
    return std::nullopt;
  }

  void add(std::string problem) { problems_.push_back(std::move(problem)); }

 private:
  const json* record_;  // Never null; outlives the reader.
  std::vector<std::string> problems_;
};

std::string join(const std::vector<std::string>& parts) {
  std::string joined;
  for (const std::string& part : parts) {
    if (!joined.empty()) {
      joined += "; ";
    }
    joined += part;
  }
  return joined;
}

// Reads one record. Returns the element set, or fills error and returns nothing.
std::optional<ElementSet> read_record(const json& record, RecordError& error) {
  if (!record.is_object()) {
    error.message = "record is " + describe_type(record) + ", expected an object";
    return std::nullopt;
  }
  RecordReader reader(record);

  const auto name = reader.string("OBJECT_NAME", Need::kOptional);
  const auto object_id = reader.string("OBJECT_ID", Need::kOptional);
  const auto catalog = reader.integer("NORAD_CAT_ID", Need::kRequired, 1, kMaxCatalogNumber);
  error.object_name = name.value_or("");
  error.object_id = object_id.value_or("");
  error.norad_cat_id = catalog;

  // SGP4-XP element sets (type 4) look the same but give wrong positions in plain SGP4.
  const auto ephemeris_type =
      reader.integer("EPHEMERIS_TYPE", Need::kOptional, std::numeric_limits<std::int64_t>::min(),
                     std::numeric_limits<std::int64_t>::max());
  if (ephemeris_type && *ephemeris_type != 0) {
    reader.add("EPHEMERIS_TYPE = " + format_number(*ephemeris_type) + " is not 0 (SGP4)");
  }

  ElementSet set;
  if (const auto epoch_text = reader.string("EPOCH", Need::kRequired)) {
    if (auto problem = parse_epoch(*epoch_text, set.epoch)) {
      reader.add(std::move(*problem));
    }
  }

  // number() only returns finite values, so plain comparisons are safe here.
  const auto mean_motion = reader.number("MEAN_MOTION", Need::kRequired);
  if (mean_motion && *mean_motion <= 0.0) {
    reader.add("MEAN_MOTION = " + format_number(*mean_motion) + " is not positive");
  }
  const auto eccentricity = reader.number("ECCENTRICITY", Need::kRequired);
  if (eccentricity && (*eccentricity < 0.0 || *eccentricity >= 1.0)) {
    reader.add("ECCENTRICITY = " + format_number(*eccentricity) + " is outside [0, 1)");
  }
  const auto inclination_deg = reader.number("INCLINATION", Need::kRequired);
  if (inclination_deg && (*inclination_deg < 0.0 || *inclination_deg > 180.0)) {
    reader.add("INCLINATION = " + format_number(*inclination_deg) + " is outside [0, 180] degrees");
  }
  const auto raan_deg = reader.number("RA_OF_ASC_NODE", Need::kRequired);
  const auto arg_perigee_deg = reader.number("ARG_OF_PERICENTER", Need::kRequired);
  const auto mean_anomaly_deg = reader.number("MEAN_ANOMALY", Need::kRequired);
  const auto bstar = reader.number("BSTAR", Need::kRequired);
  const auto mean_motion_dot = reader.number("MEAN_MOTION_DOT", Need::kOptional);
  const auto mean_motion_ddot = reader.number("MEAN_MOTION_DDOT", Need::kOptional);

  // Without problems every required value is present; the explicit checks say so to the
  // reader (and to clang-tidy) rather than leaving it implied.
  if (!reader.problems().empty() || !catalog || !mean_motion || !eccentricity || !inclination_deg ||
      !raan_deg || !arg_perigee_deg || !mean_anomaly_deg || !bstar) {
    error.message = join(reader.problems());
    return std::nullopt;
  }

  set.object_name = name.value_or("");
  set.object_id = object_id.value_or("");
  set.norad_cat_id = *catalog;
  set.mean_motion_rev_per_day = *mean_motion;
  set.eccentricity = *eccentricity;
  set.inclination_rad = deg_to_rad(*inclination_deg);
  set.raan_rad = deg_to_rad(*raan_deg);
  set.arg_perigee_rad = deg_to_rad(*arg_perigee_deg);
  set.mean_anomaly_rad = deg_to_rad(*mean_anomaly_deg);
  set.bstar_per_earth_radius = *bstar;
  set.mean_motion_dot_rev_per_day2 = mean_motion_dot.value_or(0.0);
  set.mean_motion_ddot_rev_per_day3 = mean_motion_ddot.value_or(0.0);
  return set;
}

LoadResult load_document(const json& document) {
  LoadResult result;
  if (!document.is_array()) {
    result.file_error =
        "top-level JSON value is " + describe_type(document) + ", expected an array";
    return result;
  }
  result.element_sets.reserve(document.size());
  std::size_t index = 0;
  for (const json& record : document) {
    RecordError error;
    error.index = index++;
    if (auto set = read_record(record, error)) {
      result.element_sets.push_back(std::move(*set));
    } else {
      result.record_errors.push_back(std::move(error));
    }
  }
  return result;
}

LoadResult file_failure(std::string message) {
  LoadResult result;
  result.file_error = std::move(message);
  return result;
}

}  // namespace

LoadResult load_omm_json(std::istream& in) noexcept {
  try {
    if (!in) {
      return file_failure("input stream is not readable");
    }
    // The throwing parse is used for its message (line, column, what was expected); the
    // exception is caught right here.
    const json document = json::parse(in);
    return load_document(document);
  } catch (const json::parse_error& e) {
    return file_failure(std::string("not valid JSON: ") + e.what());
  } catch (const json::out_of_range& e) {
    // nlohmann stops the whole parse on a number that overflows a double (e.g. 1e400), so
    // this cannot be reported per record.
    return file_failure(std::string("number out of range: ") + e.what());
  } catch (const std::exception& e) {
    return file_failure(std::string("could not load OMM JSON: ") + e.what());
  }
}

LoadResult load_omm_json(const std::filesystem::path& path) noexcept {
  try {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
      return file_failure(path.string() + " is a directory");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return file_failure("cannot open " + path.string());
    }
    LoadResult result = load_omm_json(file);
    if (result.file_error) {
      result.file_error = path.string() + ": " + *result.file_error;
    }
    return result;
  } catch (const std::exception& e) {
    return file_failure(path.string() + ": could not load OMM JSON: " + e.what());
  }
}

}  // namespace satsim::orbit
