#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <istream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "load_support.hpp"
#include "satsim/ground/loaders.hpp"

namespace satsim::ground {

namespace {

using detail::quote;
using nlohmann::json;

constexpr double kMaxLatitudeDeg = 90.0;
constexpr double kMaxLongitudeDeg = 180.0;

// Shortest round-trip text for a number, independent of the C locale.
std::string format_number(double value) {
  std::array<char, 32> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("?");
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

enum class Need : std::uint8_t { kRequired, kOptional };

// Validates one JSON object and collects every problem found, so a user fixing a file sees
// all of them at once.
class RecordReader {
 public:
  explicit RecordReader(const json& record) : record_(&record) {}

  [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }
  void add(std::string problem) { problems_.push_back(std::move(problem)); }

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

  // A string; when required it must also be non-empty.
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
    auto text = value->get<std::string>();
    if (need == Need::kRequired && text.empty()) {
      problems_.push_back(std::string(key) + " is empty");
      return std::nullopt;
    }
    return text;
  }

  // A finite number within [-limit, limit].
  std::optional<double> number_within(const char* key, double limit) {
    const json* value = find(key, Need::kRequired);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (!value->is_number()) {
      problems_.push_back(std::string(key) + " is " + describe_type(*value) +
                          ", expected a number");
      return std::nullopt;
    }
    const auto number = value->get<double>();
    // JSON has no inf or NaN literal, so the isfinite check only guards the contract.
    if (!std::isfinite(number) || number < -limit || number > limit) {
      problems_.push_back(std::string(key) + " = " + format_number(number) + " is outside [" +
                          format_number(-limit) + ", " + format_number(limit) + "] degrees");
      return std::nullopt;
    }
    return number;
  }

 private:
  const json* record_;  // Never null; outlives the reader.
  std::vector<std::string> problems_;
};

std::string join_problems(const std::vector<std::string>& problems) {
  return detail::join(problems, "; ");
}

template <typename T>
using Indexed = std::vector<std::pair<std::size_t, T>>;  // Array index and record.

// Reads every element of array with read(record, error) -> optional<T>. Returns the good ones
// with their indices; appends the bad ones to errors.
template <typename T, typename Read>
Indexed<T> read_array(const json& array, Read read, std::vector<RecordError>& errors) {
  Indexed<T> good;
  std::size_t index = 0;
  for (const json& record : array) {
    RecordError error;
    error.index = index;
    if (!record.is_object()) {
      error.message = "record is " + describe_type(record) + ", expected an object";
      errors.push_back(std::move(error));
    } else if (auto item = read(record, error)) {
      good.emplace_back(index, std::move(*item));
    } else {
      errors.push_back(std::move(error));
    }
    ++index;
  }
  return good;
}

// Moves every record of good into records, except those whose key(record) is shared with
// another, which become errors. Then sorts errors by index.
template <typename T, typename Key>
void keep_unique(Indexed<T>& good, Key key, std::string_view key_label, std::vector<T>& records,
                 std::vector<RecordError>& errors) {
  std::vector<std::string> keys;
  std::vector<std::size_t> positions;
  for (const auto& [position, item] : good) {
    keys.push_back(key(item));
    positions.push_back(position);
  }
  const auto duplicates = detail::find_duplicates(
      keys, positions, "index",
      [key_label](const std::string& k) { return std::string(key_label) + " " + quote(k); });
  for (auto& [position, item] : good) {
    const auto it = duplicates.find(position);
    if (it == duplicates.end()) {
      records.push_back(std::move(item));
      continue;
    }
    RecordError error;
    error.index = position;
    error.name = key(item);
    error.message = it->second;
    errors.push_back(std::move(error));
  }
  std::sort(errors.begin(), errors.end(),
            [](const RecordError& a, const RecordError& b) { return a.index < b.index; });
}

template <typename T>
void keep_all(Indexed<T>& good, std::vector<T>& records) {
  for (auto& entry : good) {
    records.push_back(std::move(entry.second));
  }
}

std::optional<GroundSite> read_site(RecordReader& reader, const std::string& name) {
  const auto lat = reader.number_within("lat", kMaxLatitudeDeg);
  const auto lon = reader.number_within("lon", kMaxLongitudeDeg);
  if (!lat || !lon) {
    return std::nullopt;
  }
  return make_ground_site(name, *lat, *lon);
}

std::optional<City> read_city(const json& record, RecordError& error) {
  RecordReader reader(record);
  const auto name = reader.string("name", Need::kRequired);
  error.name = name.value_or("");
  const auto country = reader.string("country", Need::kRequired);
  const auto region = reader.string("region", Need::kRequired);
  const auto geofeed_city = reader.string("geofeed_city", Need::kRequired);
  auto site = read_site(reader, name.value_or(""));
  if (!reader.problems().empty() || !name || !country || !region || !geofeed_city || !site) {
    error.message = join_problems(reader.problems());
    return std::nullopt;
  }
  City city;
  city.site = std::move(*site);
  city.country = *country;
  city.region = *region;
  city.geofeed_city = *geofeed_city;
  return city;
}

std::optional<CityPair> read_pair(const json& record, const std::set<std::string>& city_names,
                                  RecordError& error) {
  RecordReader reader(record);
  const auto from = reader.string("from", Need::kRequired);
  const auto to = reader.string("to", Need::kRequired);
  const auto role = reader.string("role", Need::kRequired);
  const auto why = reader.string("why", Need::kOptional);
  error.name = from.value_or("") + " - " + to.value_or("");
  for (const auto* end : {&from, &to}) {
    if (*end && city_names.count(**end) == 0) {
      reader.add("city " + quote(**end) + " is not a loaded city");
    }
  }
  if (from && to && *from == *to) {
    reader.add("from and to are the same city");
  }
  std::optional<PairRole> pair_role;
  if (role) {
    if (*role == "study") {
      pair_role = PairRole::kStudy;
    } else if (*role == "validation") {
      pair_role = PairRole::kValidation;
    } else {
      reader.add("role " + quote(*role) + R"( is not "study" or "validation")");
    }
  }
  if (!reader.problems().empty() || !from || !to || !pair_role) {
    error.message = join_problems(reader.problems());
    return std::nullopt;
  }
  CityPair pair;
  pair.from = *from;
  pair.to = *to;
  pair.role = *pair_role;
  pair.why = why.value_or("");
  return pair;
}

std::optional<Gateway> read_gateway(const json& record, RecordError& error) {
  RecordReader reader(record);
  const auto name = reader.string("name", Need::kRequired);
  error.name = name.value_or("");
  const auto status = reader.string("status", Need::kRequired);
  auto site = read_site(reader, name.value_or(""));
  std::optional<GatewayStatus> gateway_status;
  if (status) {
    if (*status == "operational") {
      gateway_status = GatewayStatus::kOperational;
    } else if (*status == "planned") {
      gateway_status = GatewayStatus::kPlanned;
    } else {
      reader.add("status " + quote(*status) + R"( is not "operational" or "planned")");
    }
  }
  if (!reader.problems().empty() || !name || !gateway_status || !site) {
    error.message = join_problems(reader.problems());
    return std::nullopt;
  }
  Gateway gateway;
  gateway.site = std::move(*site);
  gateway.status = *gateway_status;
  return gateway;
}

std::optional<Pop> read_pop(const json& record, RecordError& error) {
  RecordReader reader(record);
  const auto code = reader.string("code", Need::kRequired);
  error.name = code.value_or("");
  const auto city = reader.string("city", Need::kRequired);
  const auto country = reader.string("country", Need::kRequired);
  const auto coordinates_from = reader.string("coordinates_from", Need::kRequired);
  auto site = read_site(reader, city.value_or(""));

  std::optional<std::vector<std::string>> routers;
  if (const json* value = reader.find("routers", Need::kRequired)) {
    if (!value->is_array() || value->empty()) {
      reader.add("routers is " + describe_type(*value) + ", expected a non-empty array");
    } else {
      routers.emplace();
      for (const json& router : *value) {
        if (!router.is_string() || router.get_ref<const std::string&>().empty()) {
          reader.add("routers has an entry that is not a non-empty string");
          routers.reset();
          break;
        }
        routers->push_back(router.get<std::string>());
      }
    }
  }
  if (!reader.problems().empty() || !code || !city || !country || !coordinates_from || !site ||
      !routers) {
    error.message = join_problems(reader.problems());
    return std::nullopt;
  }
  Pop pop;
  pop.code = *code;
  pop.routers = std::move(*routers);
  pop.city = *city;
  pop.country = *country;
  pop.coordinates_from = *coordinates_from;
  pop.site = std::move(*site);
  return pop;
}

// Parses the whole stream; json::parse throws on bad input, caught by the callers.
json parse(std::istream& in) { return json::parse(in); }

template <typename Result, typename Body>
Result guarded(std::istream& in, const char* what, Body body) noexcept {
  try {
    if (!in) {
      return detail::file_failure<Result>("input stream is not readable");
    }
    return body(parse(in));
  } catch (const json::parse_error& e) {
    return detail::file_failure<Result>(std::string("not valid JSON: ") + e.what());
  } catch (const json::out_of_range& e) {
    return detail::file_failure<Result>(std::string("number out of range: ") + e.what());
  } catch (const std::exception& e) {
    return detail::file_failure<Result>(std::string("could not load ") + what + ": " + e.what());
  }
}

template <typename T>
JsonLoadResult<T> expect_array(const json& document) {
  JsonLoadResult<T> result;
  if (!document.is_array()) {
    result.file_error =
        "top-level JSON value is " + describe_type(document) + ", expected an array";
  }
  return result;
}

}  // namespace

CitiesLoadResult load_cities(std::istream& in) noexcept {
  return guarded<CitiesLoadResult>(in, "cities", [](const json& document) {
    CitiesLoadResult result;
    if (!document.is_object()) {
      result.file_error =
          "top-level JSON value is " + describe_type(document) + ", expected an object";
      return result;
    }
    for (const char* key : {"cities", "pairs"}) {
      const auto it = document.find(key);
      if (it == document.end()) {
        result.file_error = std::string("\"") + key + "\" missing";
        return result;
      }
      if (!it->is_array()) {
        result.file_error =
            std::string("\"") + key + "\" is " + describe_type(*it) + ", expected an array";
        return result;
      }
    }
    auto cities = read_array<City>(document.at("cities"), read_city, result.city_errors);
    keep_unique(
        cities, [](const City& city) { return city.site.name; }, "name", result.cities,
        result.city_errors);
    std::set<std::string> names;
    for (const City& city : result.cities) {
      names.insert(city.site.name);
    }
    auto pairs = read_array<CityPair>(
        document.at("pairs"),
        [&names](const json& record, RecordError& error) {
          return read_pair(record, names, error);
        },
        result.pair_errors);
    keep_all(pairs, result.pairs);
    return result;
  });
}

CitiesLoadResult load_cities(const std::filesystem::path& path) noexcept {
  return detail::load_from_path<CitiesLoadResult>(path,
                                                  [](std::istream& in) { return load_cities(in); });
}

JsonLoadResult<Gateway> load_gateways(std::istream& in) noexcept {
  return guarded<JsonLoadResult<Gateway>>(in, "gateways", [](const json& document) {
    auto result = expect_array<Gateway>(document);
    if (!result.file_error) {
      auto gateways = read_array<Gateway>(document, read_gateway, result.record_errors);
      keep_all(gateways, result.records);
    }
    return result;
  });
}

JsonLoadResult<Gateway> load_gateways(const std::filesystem::path& path) noexcept {
  return detail::load_from_path<JsonLoadResult<Gateway>>(
      path, [](std::istream& in) { return load_gateways(in); });
}

JsonLoadResult<Pop> load_pop_codes(std::istream& in) noexcept {
  return guarded<JsonLoadResult<Pop>>(in, "PoP codes", [](const json& document) {
    auto result = expect_array<Pop>(document);
    if (!result.file_error) {
      auto pops = read_array<Pop>(document, read_pop, result.record_errors);
      keep_unique(
          pops, [](const Pop& pop) { return pop.code; }, "code", result.records,
          result.record_errors);
    }
    return result;
  });
}

JsonLoadResult<Pop> load_pop_codes(const std::filesystem::path& path) noexcept {
  return detail::load_from_path<JsonLoadResult<Pop>>(
      path, [](std::istream& in) { return load_pop_codes(in); });
}

}  // namespace satsim::ground
