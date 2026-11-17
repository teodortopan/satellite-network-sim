#include "satsim/orbit/omm_loader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using satsim::orbit::ElementSet;
using satsim::orbit::load_omm_json;
using satsim::orbit::LoadResult;
using satsim::time::CivilTime;

constexpr const char* kFixturePath = "tests/orbit/data/omm_fixture.json";

// deg_to_rad multiplies by a rounded constant, so results are within a few ULPs of the
// exact value; at most 2*pi that is under 4e-15.
constexpr double kRadTolerance = 1e-14;

// STARLINK-1008, copied verbatim from data/starlink/2026-11-17/gp.json (first record of the
// fixture). Validation tests change one field at a time.
constexpr std::string_view kGoodRecord =
    R"({"OBJECT_NAME":"STARLINK-1008","OBJECT_ID":"2019-074B","EPOCH":"2026-11-16T23:06:34.909056",)"
    R"("MEAN_MOTION":15.70448124,"ECCENTRICITY":0.00027374,"INCLINATION":53.1456,)"
    R"("RA_OF_ASC_NODE":44.5949,"ARG_OF_PERICENTER":7.7459,"MEAN_ANOMALY":352.3601,)"
    R"("EPHEMERIS_TYPE":0,"CLASSIFICATION_TYPE":"U","NORAD_CAT_ID":44714,"ELEMENT_SET_NO":999,)"
    R"("REV_AT_EPOCH":38808,"BSTAR":0.0014401987,"MEAN_MOTION_DOT":0.00183364,)"
    R"("MEAN_MOTION_DDOT":0})";

// Position of the value of "key" in kGoodRecord: [begin, end). No value in it contains a
// comma, so a value ends at the next ',' or '}'.
std::pair<std::size_t, std::size_t> value_span(const std::string& record, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t key_pos = record.find(needle);
  EXPECT_NE(key_pos, std::string::npos) << key;
  const std::size_t begin = key_pos + needle.size();
  const std::size_t end = record.find_first_of(",}", begin);
  return {begin, end};
}

// kGoodRecord with key's value replaced by raw JSON text.
std::string with(const std::string& key, const std::string& raw_value) {
  std::string record(kGoodRecord);
  const auto [begin, end] = value_span(record, key);
  record.replace(begin, end - begin, raw_value);
  return record;
}

// kGoodRecord without key.
std::string without(const std::string& key) {
  std::string record(kGoodRecord);
  const std::string needle = "\"" + key + "\":";
  const std::size_t begin = record.find(needle);
  const std::size_t end = value_span(record, key).second;
  if (record[end] == ',') {
    record.erase(begin, end + 1 - begin);  // The key and the comma after it.
  } else {
    record.erase(begin - 1, end - (begin - 1));  // Last key: the comma before it.
  }
  return record;
}

LoadResult load_text(const std::string& text) {
  std::istringstream in(text);
  return load_omm_json(in);
}

LoadResult load_record(const std::string& record) { return load_text("[" + record + "]"); }

// Loads one record and returns the error message, or "<accepted>".
std::string rejection(const std::string& record) {
  const LoadResult result = load_record(record);
  EXPECT_FALSE(result.file_error.has_value());
  if (result.record_errors.size() != 1) {
    EXPECT_EQ(result.element_sets.size(), 1U);
    return "<accepted>";
  }
  EXPECT_TRUE(result.element_sets.empty());
  EXPECT_EQ(result.record_errors[0].index, 0U);
  return result.record_errors[0].message;
}

// Loads one record that must be accepted and returns it.
ElementSet accepted(const std::string& record) {
  const LoadResult result = load_record(record);
  EXPECT_FALSE(result.file_error.has_value());
  EXPECT_TRUE(result.record_errors.empty())
      << (result.record_errors.empty() ? "" : result.record_errors[0].message);
  return result.element_sets.empty() ? ElementSet{} : result.element_sets[0];
}

// ---- The fixture: real records, exact values ----

TEST(OmmLoaderTest, FixtureLoadsEveryRecordInOrder) {
  const LoadResult result = load_omm_json(std::filesystem::path(kFixturePath));
  ASSERT_FALSE(result.file_error.has_value()) << result.file_error.value_or("");
  EXPECT_TRUE(result.record_errors.empty());
  ASSERT_EQ(result.element_sets.size(), 6U);
  constexpr std::array<std::int64_t, 6> kCatalog{44714, 100001, 55278, 44753, 799501782, 799501787};
  for (std::size_t i = 0; i < kCatalog.size(); ++i) {
    EXPECT_EQ(result.element_sets[i].norad_cat_id, kCatalog[i]) << "record " << i;
  }
}

// Fixture record 0, from gp.json. Expected angles, computed to 40 digits with pi:
//   53.1456 deg  * pi/180 = 0.92756570294789841963 rad
//   44.5949 deg  * pi/180 = 0.77832783459761928050 rad
//   7.7459 deg   * pi/180 = 0.13519145853022877470 rad
//   352.3601 deg * pi/180 = 6.1498438976564716916 rad
TEST(OmmLoaderTest, StandardRecordExactValues) {
  const LoadResult result = load_omm_json(std::filesystem::path(kFixturePath));
  ASSERT_GE(result.element_sets.size(), 1U);
  const ElementSet& set = result.element_sets[0];
  EXPECT_EQ(set.object_name, "STARLINK-1008");
  EXPECT_EQ(set.object_id, "2019-074B");
  EXPECT_EQ(set.norad_cat_id, 44714);
  EXPECT_EQ(set.epoch, (CivilTime{2026, 11, 16, 23, 6, 34.909056}));
  EXPECT_EQ(set.mean_motion_rev_per_day, 15.70448124);
  EXPECT_EQ(set.eccentricity, 0.00027374);
  EXPECT_NEAR(set.inclination_rad, 0.92756570294789841963, kRadTolerance);
  EXPECT_NEAR(set.raan_rad, 0.77832783459761928050, kRadTolerance);
  EXPECT_NEAR(set.arg_perigee_rad, 0.13519145853022877470, kRadTolerance);
  EXPECT_NEAR(set.mean_anomaly_rad, 6.1498438976564716916, kRadTolerance);
  EXPECT_EQ(set.bstar_per_earth_radius, 0.0014401987);
  EXPECT_EQ(set.mean_motion_dot_rev_per_day2, 0.00183364);
  EXPECT_EQ(set.mean_motion_ddot_rev_per_day3, 0.0);
}

// Fixture record 4, from supgp.json: a nine-digit catalog number, eccentricity written as
// 1.0e-7, negative BSTAR and MEAN_MOTION_DOT, and extra keys (RMS, DATA_SOURCE). Angles:
//   30.5013 deg  * pi/180 = 0.53234811113854644725 rad
//   146.7741 deg * pi/180 = 2.5616913016514092876 rad
//   226.1298 deg * pi/180 = 3.9467095468762734836 rad
//   91.9113 deg  * pi/180 = 1.6041548047882642404 rad
TEST(OmmLoaderTest, SupplementalRecordExactValues) {
  const LoadResult result = load_omm_json(std::filesystem::path(kFixturePath));
  ASSERT_GE(result.element_sets.size(), 5U);
  const ElementSet& set = result.element_sets[4];
  EXPECT_EQ(set.object_name, "STARLINK-40096");
  EXPECT_EQ(set.object_id, "2026-256B");
  EXPECT_EQ(set.norad_cat_id, 799501782);
  EXPECT_EQ(set.epoch, (CivilTime{2026, 11, 17, 17, 37, 42.000010}));
  EXPECT_EQ(set.mean_motion_rev_per_day, 15.91820326);
  EXPECT_EQ(set.eccentricity, 1.0e-7);
  EXPECT_NEAR(set.inclination_rad, 0.53234811113854644725, kRadTolerance);
  EXPECT_NEAR(set.raan_rad, 2.5616913016514092876, kRadTolerance);
  EXPECT_NEAR(set.arg_perigee_rad, 3.9467095468762734836, kRadTolerance);
  EXPECT_NEAR(set.mean_anomaly_rad, 1.6041548047882642404, kRadTolerance);
  EXPECT_EQ(set.bstar_per_earth_radius, -0.0061145);
  EXPECT_EQ(set.mean_motion_dot_rev_per_day2, -0.020182);
  EXPECT_EQ(set.mean_motion_ddot_rev_per_day3, 0.0);
}

// CelesTrak writes whole numbers without a decimal point ("INCLINATION":70, "ECCENTRICITY":0),
// so JSON integers must be accepted wherever a number is expected.
// 70 deg * pi/180 = 1.2217304763960307031 rad.
TEST(OmmLoaderTest, IntegerWrittenNumbersAreAccepted) {
  const LoadResult result = load_omm_json(std::filesystem::path(kFixturePath));
  ASSERT_EQ(result.element_sets.size(), 6U);
  EXPECT_EQ(result.element_sets[1].norad_cat_id, 100001);  // Six digits.
  EXPECT_NEAR(result.element_sets[2].inclination_rad, 1.2217304763960307031, kRadTolerance);
  EXPECT_EQ(result.element_sets[3].mean_motion_ddot_rev_per_day3, 0.00016631);
  EXPECT_EQ(result.element_sets[5].eccentricity, 0.0);
}

// ---- Per-record validation ----

TEST(OmmLoaderTest, GoodRecordHelperIsAccepted) {
  EXPECT_EQ(rejection(std::string(kGoodRecord)), "<accepted>");
}

TEST(OmmLoaderTest, EachRequiredFieldMissing) {
  constexpr std::array kRequired{"NORAD_CAT_ID",      "EPOCH",        "MEAN_MOTION",
                                 "ECCENTRICITY",      "INCLINATION",  "RA_OF_ASC_NODE",
                                 "ARG_OF_PERICENTER", "MEAN_ANOMALY", "BSTAR"};
  for (const char* key : kRequired) {
    EXPECT_EQ(rejection(without(key)), std::string(key) + " missing");
  }
}

TEST(OmmLoaderTest, OptionalFieldsDefaultWhenMissing) {
  EXPECT_EQ(accepted(without("OBJECT_NAME")).object_name, "");
  EXPECT_EQ(accepted(without("OBJECT_ID")).object_id, "");
  EXPECT_EQ(accepted(without("MEAN_MOTION_DOT")).mean_motion_dot_rev_per_day2, 0.0);
  EXPECT_EQ(accepted(without("MEAN_MOTION_DDOT")).mean_motion_ddot_rev_per_day3, 0.0);
  EXPECT_EQ(accepted(without("EPHEMERIS_TYPE")).norad_cat_id, 44714);
}

TEST(OmmLoaderTest, UnusedFieldsAreIgnoredEvenIfOdd) {
  EXPECT_EQ(rejection(with("CLASSIFICATION_TYPE", "null")), "<accepted>");
  EXPECT_EQ(rejection(with("REV_AT_EPOCH", R"("many")")), "<accepted>");
}

TEST(OmmLoaderTest, NumbersSentAsStringsAreRejected) {
  EXPECT_EQ(rejection(with("ECCENTRICITY", R"("0.00027374")")),
            "ECCENTRICITY is a string, expected a number");
  EXPECT_EQ(rejection(with("MEAN_MOTION_DOT", R"("0.00183364")")),
            "MEAN_MOTION_DOT is a string, expected a number");
  EXPECT_EQ(rejection(with("NORAD_CAT_ID", R"("44714")")),
            "NORAD_CAT_ID is a string, expected an integer");
}

TEST(OmmLoaderTest, OtherWrongTypes) {
  EXPECT_EQ(rejection(with("BSTAR", "null")), "BSTAR is null, expected a number");
  EXPECT_EQ(rejection(with("INCLINATION", "true")), "INCLINATION is a boolean, expected a number");
  EXPECT_EQ(rejection(with("MEAN_ANOMALY", "[1]")), "MEAN_ANOMALY is an array, expected a number");
  EXPECT_EQ(rejection(with("RA_OF_ASC_NODE", R"({"deg":1})")),
            "RA_OF_ASC_NODE is an object, expected a number");
  EXPECT_EQ(rejection(with("EPOCH", "20261116")), "EPOCH is a number, expected a string");
  EXPECT_EQ(rejection(with("OBJECT_NAME", "1008")), "OBJECT_NAME is a number, expected a string");
  EXPECT_EQ(rejection(with("OBJECT_ID", "null")), "OBJECT_ID is null, expected a string");
  EXPECT_EQ(rejection(with("EPHEMERIS_TYPE", "0.0")),
            "EPHEMERIS_TYPE is a number, expected an integer");
}

TEST(OmmLoaderTest, CatalogNumberMustBeAnInteger) {
  EXPECT_EQ(rejection(with("NORAD_CAT_ID", "44714.0")),
            "NORAD_CAT_ID is a number, expected an integer");
  EXPECT_EQ(rejection(with("NORAD_CAT_ID", "4.4714e4")),
            "NORAD_CAT_ID is a number, expected an integer");
  // Beyond 64 bits: the JSON parser falls back to a floating-point number.
  EXPECT_EQ(rejection(with("NORAD_CAT_ID", "100000000000000000000")),
            "NORAD_CAT_ID is a number, expected an integer");
}

TEST(OmmLoaderTest, CatalogNumberRange) {
  EXPECT_EQ(accepted(with("NORAD_CAT_ID", "1")).norad_cat_id, 1);
  EXPECT_EQ(accepted(with("NORAD_CAT_ID", "999999999")).norad_cat_id, 999999999);
  EXPECT_EQ(rejection(with("NORAD_CAT_ID", "0")), "NORAD_CAT_ID = 0 is outside [1, 999999999]");
  EXPECT_EQ(rejection(with("NORAD_CAT_ID", "-5")), "NORAD_CAT_ID = -5 is outside [1, 999999999]");
  EXPECT_EQ(rejection(with("NORAD_CAT_ID", "1000000000")),
            "NORAD_CAT_ID = 1000000000 is outside [1, 999999999]");
  EXPECT_EQ(rejection(with("NORAD_CAT_ID", "18446744073709551615")),
            "NORAD_CAT_ID = 18446744073709551615 is outside [1, 999999999]");
}

TEST(OmmLoaderTest, MeanMotionMustBePositive) {
  EXPECT_EQ(rejection(with("MEAN_MOTION", "-1")), "MEAN_MOTION = -1 is not positive");
  EXPECT_EQ(rejection(with("MEAN_MOTION", "0")), "MEAN_MOTION = 0 is not positive");
  EXPECT_EQ(rejection(with("MEAN_MOTION", "-0.0")), "MEAN_MOTION = -0 is not positive");
  // Underflows to zero when parsed.
  EXPECT_EQ(rejection(with("MEAN_MOTION", "1e-400")), "MEAN_MOTION = 0 is not positive");
  // The rule is only "positive": SGP4 itself rejects implausible orbits later.
  EXPECT_EQ(accepted(with("MEAN_MOTION", "4.9e-324")).mean_motion_rev_per_day, 4.9e-324);
}

TEST(OmmLoaderTest, EccentricityRange) {
  EXPECT_EQ(accepted(with("ECCENTRICITY", "0")).eccentricity, 0.0);
  EXPECT_EQ(accepted(with("ECCENTRICITY", "0.9999999")).eccentricity, 0.9999999);
  EXPECT_EQ(accepted(with("ECCENTRICITY", "1e-400")).eccentricity, 0.0);  // Underflow.
  EXPECT_EQ(rejection(with("ECCENTRICITY", "1")), "ECCENTRICITY = 1 is outside [0, 1)");
  EXPECT_EQ(rejection(with("ECCENTRICITY", "1.5")), "ECCENTRICITY = 1.5 is outside [0, 1)");
  EXPECT_EQ(rejection(with("ECCENTRICITY", "-1e-9")), "ECCENTRICITY = -1e-09 is outside [0, 1)");
  // Rounds to exactly 1.0 when parsed.
  EXPECT_EQ(rejection(with("ECCENTRICITY", "0.99999999999999999999")),
            "ECCENTRICITY = 1 is outside [0, 1)");
}

TEST(OmmLoaderTest, InclinationRange) {
  EXPECT_EQ(accepted(with("INCLINATION", "0")).inclination_rad, 0.0);
  // 180 deg * pi/180 = pi = 3.14159265358979323846 rad.
  EXPECT_NEAR(accepted(with("INCLINATION", "180")).inclination_rad, 3.14159265358979323846,
              kRadTolerance);
  EXPECT_EQ(rejection(with("INCLINATION", "180.0001")),
            "INCLINATION = 180.0001 is outside [0, 180] degrees");
  EXPECT_EQ(rejection(with("INCLINATION", "-0.5")),
            "INCLINATION = -0.5 is outside [0, 180] degrees");
}

// JSON has no literal for infinity; an overflowing number is the only way to get one, and
// nlohmann stops the whole parse on it. So it is a file error, not a record error.
TEST(OmmLoaderTest, OverflowingNumberIsAFileError) {
  const std::array<std::string, 4> kOverflowing{
      with("MEAN_ANOMALY", "1e400"), with("BSTAR", "-1e400"), with("MEAN_MOTION_DDOT", "1e400"),
      with("RA_OF_ASC_NODE", "1" + std::string(400, '0'))};  // Integer digits, too big.
  for (const std::string& record : kOverflowing) {
    const LoadResult result = load_text("[" + std::string(kGoodRecord) + "," + record + "]");
    ASSERT_TRUE(result.file_error.has_value());
    EXPECT_EQ(result.file_error.value_or("").rfind("number out of range: ", 0), 0U)
        << result.file_error.value_or("");
    EXPECT_NE(result.file_error.value_or("").find("number overflow"), std::string::npos)
        << result.file_error.value_or("");
    EXPECT_TRUE(result.element_sets.empty());
  }
}

TEST(OmmLoaderTest, HugeFiniteAnglesAreKept) {
  // Angles outside [0, 360) are legal input to SGP4, which reduces them itself.
  const ElementSet set = accepted(with("RA_OF_ASC_NODE", "1.7976931348623157e308"));
  EXPECT_TRUE(std::isfinite(set.raan_rad));
  EXPECT_GT(set.raan_rad, 1e306);
  EXPECT_NEAR(accepted(with("ARG_OF_PERICENTER", "-90")).arg_perigee_rad, -1.5707963267948966192,
              kRadTolerance);
}

TEST(OmmLoaderTest, EphemerisTypeMustBeSgp4) {
  EXPECT_EQ(rejection(with("EPHEMERIS_TYPE", "4")), "EPHEMERIS_TYPE = 4 is not 0 (SGP4)");
  EXPECT_EQ(rejection(with("EPHEMERIS_TYPE", "-1")), "EPHEMERIS_TYPE = -1 is not 0 (SGP4)");
}

TEST(OmmLoaderTest, AllProblemsOfARecordAreReported) {
  std::string record = with("MEAN_MOTION", "-1");
  record.replace(record.find("\"ECCENTRICITY\""), 1, "\"X_");  // Renames the key.
  EXPECT_EQ(rejection(record), "MEAN_MOTION = -1 is not positive; ECCENTRICITY missing");
}

TEST(OmmLoaderTest, RecordErrorCarriesReadableIdentity) {
  LoadResult result = load_record(with("ECCENTRICITY", "2"));
  ASSERT_EQ(result.record_errors.size(), 1U);
  EXPECT_EQ(result.record_errors[0].object_name, "STARLINK-1008");
  EXPECT_EQ(result.record_errors[0].object_id, "2019-074B");
  EXPECT_EQ(result.record_errors[0].norad_cat_id, 44714);

  result = load_record(with("NORAD_CAT_ID", "0"));
  ASSERT_EQ(result.record_errors.size(), 1U);
  EXPECT_EQ(result.record_errors[0].object_name, "STARLINK-1008");
  EXPECT_FALSE(result.record_errors[0].norad_cat_id.has_value());
}

TEST(OmmLoaderTest, RecordThatIsNotAnObject) {
  EXPECT_EQ(rejection("5"), "record is a number, expected an object");
  EXPECT_EQ(rejection("null"), "record is null, expected an object");
  EXPECT_EQ(rejection("[" + std::string(kGoodRecord) + "]"),
            "record is an array, expected an object");
  EXPECT_EQ(rejection(R"("STARLINK-1008")"), "record is a string, expected an object");
}

// ---- EPOCH ----

TEST(OmmLoaderTest, EpochAcceptedForms) {
  EXPECT_EQ(accepted(with("EPOCH", R"("2026-11-16T23:06:34")")).epoch,
            (CivilTime{2026, 11, 16, 23, 6, 34.0}));
  EXPECT_EQ(accepted(with("EPOCH", R"("2026-11-16T23:06:34.5")")).epoch,
            (CivilTime{2026, 11, 16, 23, 6, 34.5}));
  EXPECT_EQ(accepted(with("EPOCH", R"("2024-12-31T23:59:59.999999999")")).epoch,
            (CivilTime{2024, 12, 31, 23, 59, 59.999999999}));
  EXPECT_EQ(accepted(with("EPOCH", R"("2026-01-01T00:00:00.000000")")).epoch,
            (CivilTime{2026, 1, 1, 0, 0, 0.0}));
  // Whether a date exists is checked when CivilTime becomes an instant, not here.
  EXPECT_EQ(accepted(with("EPOCH", R"("2026-02-31T00:00:00")")).epoch,
            (CivilTime{2026, 2, 31, 0, 0, 0.0}));
}

TEST(OmmLoaderTest, EpochBadFormat) {
  constexpr std::array kBad{
      "2026-11-16T23:06:34.909056Z",  // CelesTrak sends no zone designator.
      "2026-11-16 23:06:34",
      "2026-11-16T23:06:34.",
      "2026-11-16T23:06:34.1234567890",  // Ten fraction digits.
      "2026-1-16T23:06:34",
      "26-11-16T23:06:34",
      "2026-11-16T23:06",
      "",
      "2026-11-16T23:06:34.+1",
      "2026-11-16T23:06:3a",
      "+026-11-16T23:06:34",
      "2026-11-16T23:06:34.909056 ",
      "2026-11-16T23:06:34,909056",
      "2026-11-16t23:06:34",
      "2026-11-16T23:06:34e1",
      "2026-11-16T-3:06:34",
  };
  for (const char* epoch : kBad) {
    EXPECT_EQ(rejection(with("EPOCH", "\"" + std::string(epoch) + "\"")),
              "EPOCH \"" + std::string(epoch) + "\" is not YYYY-MM-DDTHH:MM:SS[.ffffff]");
  }
}

TEST(OmmLoaderTest, EpochLongJunkIsTruncatedInMessage) {
  const std::string junk(50, 'x');
  EXPECT_EQ(rejection(with("EPOCH", "\"" + junk + "\"")),
            "EPOCH \"" + std::string(40, 'x') + "...\" is not YYYY-MM-DDTHH:MM:SS[.ffffff]");
}

TEST(OmmLoaderTest, EpochFieldRanges) {
  const std::array<std::pair<const char*, const char*>, 8> kCases{{
      {"2026-00-16T23:06:34", "month 0 is outside 1-12"},
      {"2026-13-16T23:06:34", "month 13 is outside 1-12"},
      {"2026-11-00T23:06:34", "day 0 is outside 1-31"},
      {"2026-11-32T23:06:34", "day 32 is outside 1-31"},
      {"2026-11-16T24:00:00", "hour 24 is outside 0-23"},
      {"2026-11-16T23:60:00", "minute 60 is outside 0-59"},
      {"2026-11-16T23:59:60", "second 60 is outside [0, 60)"},
      {"2026-11-16T23:59:99.5", "second 99.5 is outside [0, 60)"},
  }};
  for (const auto& [epoch, problem] : kCases) {
    EXPECT_EQ(rejection(with("EPOCH", "\"" + std::string(epoch) + "\"")),
              "EPOCH \"" + std::string(epoch) + "\": " + problem);
  }
}

// ---- File level ----

TEST(OmmLoaderTest, EmptyArrayIsNotAnError) {
  const LoadResult result = load_text("[]");
  EXPECT_FALSE(result.file_error.has_value());
  EXPECT_TRUE(result.element_sets.empty());
  EXPECT_TRUE(result.record_errors.empty());
}

TEST(OmmLoaderTest, TopLevelNotAnArray) {
  EXPECT_EQ(load_text("{}").file_error, "top-level JSON value is an object, expected an array");
  EXPECT_EQ(load_text(std::string(kGoodRecord)).file_error,
            "top-level JSON value is an object, expected an array");
  EXPECT_EQ(load_text("\"x\"").file_error, "top-level JSON value is a string, expected an array");
  EXPECT_EQ(load_text("null").file_error, "top-level JSON value is null, expected an array");
  EXPECT_EQ(load_text("42").file_error, "top-level JSON value is a number, expected an array");
}

TEST(OmmLoaderTest, InvalidJsonIsAFileError) {
  std::ifstream fixture(kFixturePath, std::ios::binary);
  const std::string full((std::istreambuf_iterator<char>(fixture)),
                         std::istreambuf_iterator<char>());
  ASSERT_GT(full.size(), 1000U);

  const std::array<std::string, 9> kBad{
      "",
      "   \n",
      full.substr(0, full.size() / 2),  // Truncated mid-record.
      full.substr(0, full.size() - 3),  // Missing the closing bracket.
      "[] trailing",
      std::string("\x00\xff\xfe garbage", 11),
      "[" + with("BSTAR", "NaN") + "]",  // Not JSON, although some writers emit it.
      "[" + with("BSTAR", "Infinity") + "]",
      "[" + std::string(kGoodRecord) + ",]",  // Trailing comma.
  };
  for (const std::string& text : kBad) {
    const LoadResult result = load_text(text);
    ASSERT_TRUE(result.file_error.has_value()) << text.substr(0, 60);
    EXPECT_EQ(result.file_error.value_or("").rfind("not valid JSON: ", 0), 0U)
        << result.file_error.value_or("");
    EXPECT_TRUE(result.element_sets.empty());
    EXPECT_TRUE(result.record_errors.empty());
  }
}

TEST(OmmLoaderTest, UnreadableStream) {
  std::istringstream in("[]");
  in.setstate(std::ios::badbit);
  EXPECT_EQ(load_omm_json(in).file_error, "input stream is not readable");
}

TEST(OmmLoaderTest, MissingFile) {
  const std::filesystem::path path = "tests/orbit/data/does_not_exist.json";
  EXPECT_EQ(load_omm_json(path).file_error, "cannot open tests/orbit/data/does_not_exist.json");
}

TEST(OmmLoaderTest, DirectoryInsteadOfFile) {
  EXPECT_EQ(load_omm_json(std::filesystem::path("tests/orbit/data")).file_error,
            "tests/orbit/data is a directory");
}

TEST(OmmLoaderTest, FileErrorsNameThePath) {
  // A file that exists but is not JSON.
  const LoadResult result = load_omm_json(std::filesystem::path("tests/orbit/CMakeLists.txt"));
  ASSERT_TRUE(result.file_error.has_value());
  EXPECT_EQ(result.file_error.value_or("").rfind("tests/orbit/CMakeLists.txt: not valid JSON: ", 0),
            0U)
      << result.file_error.value_or("");
}

TEST(OmmLoaderTest, MixedFileKeepsGoodRecordsAndReportsBadOnes) {
  const std::string text = "[" + std::string(kGoodRecord) + "," + without("ECCENTRICITY") + ",7," +
                           with("NORAD_CAT_ID", "55278") + "," +
                           with("EPOCH", R"("2026-11-16T25:00:00")") + "]";
  const LoadResult result = load_text(text);
  ASSERT_FALSE(result.file_error.has_value());
  ASSERT_EQ(result.element_sets.size(), 2U);
  EXPECT_EQ(result.element_sets[0].norad_cat_id, 44714);
  EXPECT_EQ(result.element_sets[1].norad_cat_id, 55278);
  ASSERT_EQ(result.record_errors.size(), 3U);
  EXPECT_EQ(result.record_errors[0].index, 1U);
  EXPECT_EQ(result.record_errors[0].message, "ECCENTRICITY missing");
  EXPECT_EQ(result.record_errors[1].index, 2U);
  EXPECT_EQ(result.record_errors[1].message, "record is a number, expected an object");
  EXPECT_FALSE(result.record_errors[1].norad_cat_id.has_value());
  EXPECT_EQ(result.record_errors[2].index, 4U);
  EXPECT_EQ(result.record_errors[2].message,
            "EPOCH \"2026-11-16T25:00:00\": hour 25 is outside 0-23");
}

}  // namespace
