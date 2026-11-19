#pragma once

// Helpers shared by the JSON and CSV loaders of the ground module. Internal: not part of
// include/satsim.

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace satsim::ground::detail {

inline constexpr std::size_t kMaxQuotedLength = 50;  // Keeps messages short on junk input.

inline std::string quote(std::string_view text) {
  if (text.size() > kMaxQuotedLength) {
    return "\"" + std::string(text.substr(0, kMaxQuotedLength)) + "...\"";
  }
  return "\"" + std::string(text) + "\"";
}

inline std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string joined;
  for (const std::string& part : parts) {
    if (!joined.empty()) {
      joined += separator;
    }
    joined += part;
  }
  return joined;
}

/// For keys[i] (the key of the item at position positions[i]), the positions of every item
/// whose key appears more than once, each with a message naming the other positions, e.g.
/// "code \"lhr\" is also at index 4". label is "index" or "line".
template <typename Key, typename Describe>
std::map<std::size_t, std::string> find_duplicates(const std::vector<Key>& keys,
                                                   const std::vector<std::size_t>& positions,
                                                   std::string_view label, Describe describe) {
  std::map<Key, std::vector<std::size_t>> by_key;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    by_key[keys[i]].push_back(positions[i]);
  }
  std::map<std::size_t, std::string> duplicates;
  for (const auto& [key, at] : by_key) {
    if (at.size() < 2) {
      continue;
    }
    for (const std::size_t position : at) {
      std::vector<std::string> others;
      for (const std::size_t other : at) {
        if (other != position) {
          others.push_back(std::to_string(other));
        }
      }
      duplicates[position] =
          describe(key) + " is also at " + std::string(label) + " " + join(others, ", ");
    }
  }
  return duplicates;
}

template <typename Result>
Result file_failure(std::string message) {
  Result result;
  result.file_error = std::move(message);
  return result;
}

/// Opens path and runs load(stream). Prefixes any file_error with the path.
template <typename Result, typename Load>
Result load_from_path(const std::filesystem::path& path, Load load) noexcept {
  try {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
      return file_failure<Result>(path.string() + " is a directory");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return file_failure<Result>("cannot open " + path.string());
    }
    Result result = load(file);
    if (result.file_error) {
      result.file_error = path.string() + ": " + *result.file_error;
    }
    return result;
  } catch (const std::exception& e) {
    return file_failure<Result>(path.string() + ": could not load: " + e.what());
  }
}

}  // namespace satsim::ground::detail
