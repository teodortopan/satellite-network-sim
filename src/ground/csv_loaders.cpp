#include <algorithm>
#include <cstddef>
#include <exception>
#include <istream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "load_support.hpp"
#include "satsim/ground/loaders.hpp"

namespace satsim::ground {

namespace {

using detail::quote;

std::vector<std::string_view> split(std::string_view line, char separator) {
  std::vector<std::string_view> fields;
  while (true) {
    const std::size_t at = line.find(separator);
    if (at == std::string_view::npos) {
      fields.push_back(line);
      return fields;
    }
    fields.push_back(line.substr(0, at));
    line.remove_prefix(at + 1);
  }
}

bool is_country_code(std::string_view text) {
  return text.size() == 2 &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
}

// Parses the block in field, or adds a problem.
std::optional<AddressBlock> read_block(std::string_view field, std::vector<std::string>& problems) {
  AddressBlockParse parsed = parse_address_block(field);
  if (!parsed.block) {
    problems.push_back("prefix " + parsed.error);
  }
  return parsed.block;
}

std::optional<PopBlock> read_pop_block(const std::vector<std::string_view>& fields,
                                       std::vector<std::string>& problems) {
  constexpr std::size_t kFields = 3;
  if (fields.size() != kFields) {
    problems.push_back("has " + std::to_string(fields.size()) + " fields, expected 3");
    return std::nullopt;
  }
  auto block = read_block(fields[0], problems);
  if (fields[1].empty()) {
    problems.emplace_back("router is empty");
  }
  if (fields[2].empty()) {
    problems.emplace_back("PoP code is empty");
  }
  if (!problems.empty() || !block) {
    return std::nullopt;
  }
  return PopBlock{
      .block = *block, .router = std::string(fields[1]), .pop_code = std::string(fields[2])};
}

std::optional<GeofeedEntry> read_geofeed_entry(const std::vector<std::string_view>& fields,
                                               std::vector<std::string>& problems) {
  constexpr std::size_t kMinFields = 4;
  constexpr std::size_t kMaxFields = 5;
  if (fields.size() < kMinFields || fields.size() > kMaxFields) {
    problems.push_back("has " + std::to_string(fields.size()) + " fields, expected 4 or 5");
    return std::nullopt;
  }
  auto block = read_block(fields[0], problems);
  if (!is_country_code(fields[1])) {
    problems.push_back("country " + quote(fields[1]) + " is not two upper-case letters");
  }
  if (!problems.empty() || !block) {
    return std::nullopt;
  }
  return GeofeedEntry{.block = *block,
                      .country = std::string(fields[1]),
                      .region = std::string(fields[2]),
                      .city = std::string(fields[3])};
}

// Reads every line with read(fields, problems) -> optional<T>, then rejects every record whose
// block is on more than one line.
template <typename T, typename Read>
CsvLoadResult<T> load_csv(std::istream& in, Read read) noexcept {
  try {
    if (!in) {
      return detail::file_failure<CsvLoadResult<T>>("input stream is not readable");
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
      return detail::file_failure<CsvLoadResult<T>>("read error");
    }
    if (text.empty()) {
      return detail::file_failure<CsvLoadResult<T>>("file is empty");
    }

    CsvLoadResult<T> result;
    std::vector<std::pair<std::size_t, T>> good;
    std::string_view rest = text;
    for (std::size_t line_number = 1; !rest.empty(); ++line_number) {
      const std::size_t newline = rest.find('\n');
      std::string_view line = rest.substr(0, newline);
      rest = newline == std::string_view::npos ? std::string_view() : rest.substr(newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }

      std::vector<std::string> problems;
      std::optional<T> record;
      if (line.empty()) {
        problems.emplace_back("empty line");
      } else if (line.find('"') != std::string_view::npos) {
        problems.emplace_back("quoted fields are not supported");
      } else {
        record = read(split(line, ','), problems);
      }
      if (record) {
        good.emplace_back(line_number, std::move(*record));
      } else {
        result.line_errors.push_back(
            LineError{.line = line_number, .message = detail::join(problems, "; ")});
      }
    }

    std::vector<AddressBlock> keys;
    std::vector<std::size_t> lines;
    for (const auto& [line, record] : good) {
      keys.push_back(record.block);
      lines.push_back(line);
    }
    const auto duplicates = detail::find_duplicates(
        keys, lines, "line", [](const AddressBlock& b) { return "block " + b.to_string(); });
    for (auto& [line, record] : good) {
      const auto it = duplicates.find(line);
      if (it == duplicates.end()) {
        result.records.push_back(std::move(record));
      } else {
        result.line_errors.push_back(LineError{.line = line, .message = it->second});
      }
    }
    std::sort(result.line_errors.begin(), result.line_errors.end(),
              [](const LineError& a, const LineError& b) { return a.line < b.line; });
    return result;
  } catch (const std::exception& e) {
    return detail::file_failure<CsvLoadResult<T>>(std::string("could not load: ") + e.what());
  }
}

}  // namespace

CsvLoadResult<PopBlock> load_starlink_pops(std::istream& in) noexcept {
  return load_csv<PopBlock>(in, read_pop_block);
}

CsvLoadResult<PopBlock> load_starlink_pops(const std::filesystem::path& path) noexcept {
  return detail::load_from_path<CsvLoadResult<PopBlock>>(
      path, [](std::istream& in) { return load_starlink_pops(in); });
}

CsvLoadResult<GeofeedEntry> load_starlink_geofeed(std::istream& in) noexcept {
  return load_csv<GeofeedEntry>(in, read_geofeed_entry);
}

CsvLoadResult<GeofeedEntry> load_starlink_geofeed(const std::filesystem::path& path) noexcept {
  return detail::load_from_path<CsvLoadResult<GeofeedEntry>>(
      path, [](std::istream& in) { return load_starlink_geofeed(in); });
}

}  // namespace satsim::ground
