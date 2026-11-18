#pragma once

// Helpers shared by the propagate tests: the fixture snapshot, scratch snapshot directories,
// and a strict reader for the CSV the program writes.

#include <gtest/gtest.h>
#include <unistd.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "propagate/arguments.hpp"
#include "propagate/csv.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"

namespace satsim::propagate::testing {

// The fixture snapshot (tests run from the repository root). gp.json holds the four standard
// records of tests/orbit/data/omm_fixture.json; supgp.json holds that file's two
// supplemental records plus the supplemental records of 44714, 100001 and 55278 from
// data/starlink/2026-11-18/supgp.json. So 44753 is only in the standard file (the fallback
// path), and 44714, 100001 and 55278 are in both, with different elements, so a wrong choice
// changes the positions. finals2000A.all is tests/time/data/finals2000A_2026q4.txt (daily
// rows 2026-10-01 to 2026-12-31).
inline const std::filesystem::path kFixtureSnapshot = "tests/propagate/data/snapshot";

// Catalogue numbers in the fixture, ascending, and the one only in the standard file.
inline const std::vector<std::int64_t> kFixtureIds = {44714,  44753,     55278,
                                                      100001, 799501782, 799501787};
inline constexpr std::int64_t kStandardOnlyId = 44753;

inline time::UtcTime utc(std::string_view iso) {
  const std::optional<time::CivilTime> civil = time::parse_iso_civil_time(iso);
  const std::optional<time::UtcTime> t =
      civil ? time::UtcTime::from_civil(*civil) : std::optional<time::UtcTime>{};
  if (!t) {
    // A test names a time that does not exist: the test itself is wrong.
    ADD_FAILURE() << "bad test time " << iso;
    std::abort();
  }
  return *t;
}

inline Options options(std::string_view start, std::int64_t duration_s, std::int64_t step_s,
                       std::filesystem::path snapshot = kFixtureSnapshot) {
  constexpr std::int64_t kNs = 1'000'000'000;
  return Options{.snapshot_dir = std::move(snapshot),
                 .start = utc(start),
                 .duration_ns = duration_s * kNs,
                 .step_ns = step_s * kNs,
                 .out = "-",
                 .ids = {}};
}

inline std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_file(const std::filesystem::path& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  ASSERT_TRUE(out.good()) << path;
}

// A fresh directory under the system temporary directory, removed with its contents on
// destruction. Named after the running test and the process, so parallel runs do not clash.
class ScratchDir {
 public:
  ScratchDir() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = std::filesystem::temp_directory_path() /
            ("satsim_propagate_" + std::string(info->test_suite_name()) + "_" + info->name() + "_" +
             std::to_string(::getpid()));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  ScratchDir(ScratchDir&&) = delete;
  ScratchDir& operator=(ScratchDir&&) = delete;
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// A snapshot directory with the given element files and the fixture's Earth-orientation file.
inline void make_snapshot(const std::filesystem::path& dir, std::string_view gp_json,
                          std::string_view supgp_json) {
  write_file(dir / "gp.json", gp_json);
  write_file(dir / "supgp.json", supgp_json);
  std::filesystem::copy_file(kFixtureSnapshot / "finals2000A.all", dir / "finals2000A.all",
                             std::filesystem::copy_options::overwrite_existing);
}

// Parses RFC 4180 CSV: fields separated by ',', records ended by '\n', a field in double
// quotes may hold ',', '"' (doubled) and line breaks. Any malformed input is a test failure.
inline std::vector<std::vector<std::string>> parse_csv(std::string_view text) {
  std::vector<std::vector<std::string>> records;
  std::vector<std::string> record;
  std::string field;
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '"') {
      ++i;
      while (true) {
        if (i >= text.size()) {
          ADD_FAILURE() << "unterminated quoted field";
          return records;
        }
        if (text[i] == '"') {
          if (i + 1 < text.size() && text[i + 1] == '"') {
            field.push_back('"');
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        field.push_back(text[i++]);
      }
      if (i < text.size() && text[i] != ',' && text[i] != '\n') {
        ADD_FAILURE() << "text after a closing quote at offset " << i;
        return records;
      }
    } else {
      while (i < text.size() && text[i] != ',' && text[i] != '\n') {
        if (text[i] == '"' || text[i] == '\r') {
          ADD_FAILURE() << "unquoted special character at offset " << i;
          return records;
        }
        field.push_back(text[i++]);
      }
    }
    record.push_back(std::move(field));
    field.clear();
    if (i >= text.size()) {
      ADD_FAILURE() << "last record not ended by a line feed";
      records.push_back(std::move(record));
      return records;
    }
    if (text[i++] == '\n') {
      records.push_back(std::move(record));
      record.clear();
    }
  }
  return records;
}

// The number a CSV field holds; the whole field must be one number.
inline double number(const std::string& field) {
  double value = 0.0;
  const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
  if (ec != std::errc{} || end != field.data() + field.size()) {
    ADD_FAILURE() << "not a number: \"" << field << "\"";
  }
  return value;
}

// Index of a column in kCsvColumns.
inline std::size_t column(std::string_view name) {
  for (std::size_t i = 0; i < kCsvColumns.size(); ++i) {
    if (kCsvColumns[i] == name) {
      return i;
    }
  }
  ADD_FAILURE() << "no column " << name;
  return 0;
}

}  // namespace satsim::propagate::testing
