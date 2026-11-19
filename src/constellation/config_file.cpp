#include "satsim/constellation/config_file.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <istream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "satsim/constellation/classification.hpp"

namespace satsim::constellation {

namespace {

using nlohmann::json;

// Reads the keys of one JSON object, remembering the first problem.
class ObjectReader {
 public:
  ObjectReader(const json& object, std::string label)
      : object_(&object), label_(std::move(label)) {}

  [[nodiscard]] const std::optional<std::string>& error() const { return error_; }

  // Fails on any key outside `allowed`.
  void only(std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, value] : object_->items()) {
      bool known = false;
      for (const std::string_view name : allowed) {
        known = known || key == name;
      }
      if (!known) {
        fail("unknown key \"" + key + "\"");
      }
    }
  }

  double number(const char* key) {
    const json* value = find(key);
    if (value == nullptr) {
      return 0.0;
    }
    if (!value->is_number()) {
      fail("\"" + std::string(key) + "\" must be a number");
      return 0.0;
    }
    return value->get<double>();
  }

  std::size_t count(const char* key) {
    const json* value = find(key);
    if (value == nullptr) {
      return 0;
    }
    if (!value->is_number_unsigned()) {
      fail("\"" + std::string(key) + "\" must be a whole number >= 0");
      return 0;
    }
    return static_cast<std::size_t>(value->get<std::uint64_t>());
  }

  std::string text(const char* key) {
    const json* value = find(key);
    if (value == nullptr) {
      return {};
    }
    if (!value->is_string()) {
      fail("\"" + std::string(key) + "\" must be a string");
      return {};
    }
    return value->get<std::string>();
  }

  void fail(std::string message) {
    if (!error_) {
      error_ = label_ + ": " + std::move(message);
    }
  }

 private:
  const json* find(const char* key) {
    const auto it = object_->find(key);
    if (it == object_->end()) {
      fail("missing \"" + std::string(key) + "\"");
      return nullptr;
    }
    return &*it;
  }

  const json* object_;
  std::string label_;
  std::optional<std::string> error_;
};

std::optional<ExclusionReason> parse_reason(std::string_view text) {
  for (const ExclusionReason reason : kAllExclusionReasons) {
    if (name_of(reason) == text) {
      return reason;
    }
  }
  return std::nullopt;
}

ClassificationConfigOrError parse_document(const json& document) {
  if (!document.is_object()) {
    return std::string("the file must hold a JSON object");
  }
  ObjectReader top(document, "file");
  top.only({"shells", "excluded_groups"});
  if (const std::optional<std::string> error = top.error()) {
    return *error;
  }
  const auto shells = document.find("shells");
  if (shells == document.end() || !shells->is_array() || shells->empty()) {
    return std::string("\"shells\" must be a non-empty array");
  }

  ClassificationConfig config = default_config();
  config.shells.clear();
  config.excluded_groups.clear();
  std::set<std::string> names;
  for (std::size_t i = 0; i < shells->size(); ++i) {
    const json& record = (*shells)[i];
    const std::string label = "shells[" + std::to_string(i) + "]";
    if (!record.is_object()) {
      return label + ": must be an object";
    }
    ObjectReader r(record, label);
    r.only({"name", "inclination_deg", "inclination_tolerance_deg", "min_height_km",
            "max_height_km", "plane_gap_threshold_deg", "min_plane_size"});
    ShellDefinition shell{.name = r.text("name"),
                          .inclination_deg = r.number("inclination_deg"),
                          .inclination_tolerance_deg = r.number("inclination_tolerance_deg"),
                          .min_height_km = r.number("min_height_km"),
                          .max_height_km = r.number("max_height_km"),
                          .plane_gap_threshold_deg = r.number("plane_gap_threshold_deg"),
                          .min_plane_size = r.count("min_plane_size")};
    if (const std::optional<std::string> error = r.error()) {
      return *error;
    }
    if (!names.insert(shell.name).second) {
      return label + ": the name \"" + shell.name + "\" is used twice";
    }
    config.shells.push_back(std::move(shell));
  }

  if (const auto groups = document.find("excluded_groups"); groups != document.end()) {
    if (!groups->is_array()) {
      return std::string("\"excluded_groups\" must be an array");
    }
    for (std::size_t i = 0; i < groups->size(); ++i) {
      const json& record = (*groups)[i];
      const std::string label = "excluded_groups[" + std::to_string(i) + "]";
      if (!record.is_object()) {
        return label + ": must be an object";
      }
      ObjectReader r(record, label);
      r.only({"name", "reason", "min_inclination_deg", "max_inclination_deg", "min_height_km",
              "max_height_km"});
      ExcludedGroup group{.name = r.text("name"),
                          .reason = ExclusionReason::kLowGroup,
                          .min_inclination_deg = r.number("min_inclination_deg"),
                          .max_inclination_deg = r.number("max_inclination_deg"),
                          .min_height_km = r.number("min_height_km"),
                          .max_height_km = r.number("max_height_km")};
      const std::string reason_text = r.text("reason");
      if (const std::optional<std::string> error = r.error()) {
        return *error;
      }
      const std::optional<ExclusionReason> reason = parse_reason(reason_text);
      if (!reason) {
        std::string message = label;
        message += ": \"" + reason_text + "\" is not an exclusion reason";
        return message;
      }
      group.reason = *reason;
      if (!names.insert(group.name).second) {
        return label + ": the name \"" + group.name + "\" is used twice";
      }
      config.excluded_groups.push_back(std::move(group));
    }
  }
  if (std::optional<std::string> error = check_config(config)) {
    return std::move(*error);
  }
  return config;
}

}  // namespace

ClassificationConfigOrError parse_classification_config(std::istream& in) {
  try {
    return parse_document(json::parse(in));
  } catch (const std::exception& e) {
    return std::string("not valid JSON: ") + e.what();
  }
}

ClassificationConfigOrError load_classification_config(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    return path.string() + " is a directory";
  }
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return "cannot open " + path.string();
  }
  ClassificationConfigOrError result = parse_classification_config(file);
  if (auto* error = std::get_if<std::string>(&result)) {
    *error = path.string() + ": " + *error;
  }
  return result;
}

}  // namespace satsim::constellation
