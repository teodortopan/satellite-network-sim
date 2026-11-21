#include "satsim/failure/failure_file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <istream>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::failure {

namespace {

using constellation::Classification;

constexpr std::string_view kHeader =
    "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s";
constexpr std::array<std::string_view, 6> kFieldNames = {
    "kind", "norad_cat_id_a", "norad_cat_id_b", "gateway", "start_utc", "length_s"};
constexpr std::size_t kGatewayField = 3;
constexpr std::size_t kMaxQuotedLength = 40;  // Keeps messages short on junk input.
constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr int kFractionDigits = 9;
// The farthest a start may lie from the run's start: the programs' longest run
// (app::kMaxDurationNs). It keeps every offset in nanoseconds well inside int64_t.
constexpr std::int64_t kMaxOffsetNs = kNanosecondsPerSecond * kNanosecondsPerSecond;

std::string quote(std::string_view text) {
  if (text.size() > kMaxQuotedLength) {
    return "\"" + std::string(text.substr(0, kMaxQuotedLength)) + "...\"";
  }
  return "\"" + std::string(text) + "\"";
}

std::string line_error(std::size_t line, const std::string& message) {
  return "line " + std::to_string(line) + ": " + message;
}

void append_padded(std::string& out, std::int64_t value, int width) {
  std::string digits = std::to_string(value);
  if (std::cmp_less(digits.size(), width)) {
    out.append(static_cast<std::size_t>(width) - digits.size(), '0');
  }
  out += digits;
}

// `utc` as YYYY-MM-DDTHH:MM:SS[.fraction], with the fewest fraction digits that are exact.
std::string format_utc(const time::UtcTime& utc) {
  const time::CivilTime civil = utc.to_civil();
  // to_civil's second is the integer nanosecond count of the minute over 1e9 (below 60, so
  // within 1e-14 s of it): rounding back recovers the count.
  const std::int64_t ns = std::llround(civil.second * static_cast<double>(kNanosecondsPerSecond));
  std::string text;
  append_padded(text, civil.year, 4);
  text += '-';
  append_padded(text, civil.month, 2);
  text += '-';
  append_padded(text, civil.day, 2);
  text += 'T';
  append_padded(text, civil.hour, 2);
  text += ':';
  append_padded(text, civil.minute, 2);
  text += ':';
  append_padded(text, ns / kNanosecondsPerSecond, 2);
  std::int64_t fraction = ns % kNanosecondsPerSecond;
  if (fraction != 0) {
    int digits = kFractionDigits;
    while (fraction % 10 == 0) {
      fraction /= 10;
      --digits;
    }
    text += '.';
    append_padded(text, fraction, digits);
  }
  return text;
}

// A whole number of nanoseconds as seconds: "1", "0.5", "1.000000001".
std::string seconds_text(std::int64_t ns) {
  std::string text = std::to_string(ns / kNanosecondsPerSecond);
  std::int64_t fraction = ns % kNanosecondsPerSecond;
  if (fraction != 0) {
    int digits = kFractionDigits;
    while (fraction % 10 == 0) {
      fraction /= 10;
      --digits;
    }
    text += '.';
    append_padded(text, fraction, digits);
  }
  return text;
}

// The shortest text that reads back as exactly `value`.
std::string double_text(double value) {
  std::array<char, 32> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("?");
}

// `utc` moved by a whole number of nanoseconds, exactly: whole seconds, then the rest (each an
// exact double, as app::time_of_step does).
time::UtcTime plus_nanoseconds(const time::UtcTime& utc, std::int64_t ns) {
  const std::int64_t whole_s = ns / kNanosecondsPerSecond;
  const std::int64_t rest_ns = ns % kNanosecondsPerSecond;
  return utc.plus_seconds(static_cast<double>(whole_s))
      .plus_seconds(static_cast<double>(rest_ns) / static_cast<double>(kNanosecondsPerSecond));
}

// The exact nanoseconds from `earlier` to `later`, or nullopt if more than kMaxOffsetNs.
// seconds_since is a double (within about 100 ns at the largest offsets); the residual from
// the instant it points at is small, so seconds_since gives it to well under a nanosecond.
std::optional<std::int64_t> nanoseconds_between(const time::UtcTime& later,
                                                const time::UtcTime& earlier) {
  const double estimate_s = later.seconds_since(earlier);
  constexpr double kLimitS = 1e9;  // kMaxOffsetNs in seconds.
  if (!(std::abs(estimate_s) <= kLimitS)) {
    return std::nullopt;
  }
  const std::int64_t estimate_ns =
      std::llround(estimate_s * static_cast<double>(kNanosecondsPerSecond));
  const time::UtcTime near = plus_nanoseconds(earlier, estimate_ns);
  const std::int64_t exact_ns =
      estimate_ns +
      std::llround(later.seconds_since(near) * static_cast<double>(kNanosecondsPerSecond));
  if (exact_ns > kMaxOffsetNs || exact_ns < -kMaxOffsetNs) {
    return std::nullopt;
  }
  return exact_ns;
}

// Decimal digits only, as an int64.
std::optional<std::int64_t> parse_catalogue_number(std::string_view text) {
  if (text.empty() ||
      !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// A decimal number (digits, at most one '.', an optional leading '-'), finite.
std::optional<double> parse_decimal(std::string_view text) {
  if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || c == '.' || c == '-';
      })) {
    return std::nullopt;
  }
  double value = 0.0;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
  if (ec != std::errc() || end != text.data() + text.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

struct Field {
  std::string text;
  bool quoted = false;
};

using FieldsOrError = std::variant<std::vector<Field>, std::string>;

// The comma-separated fields of a line. A field that starts with a double quote is quoted: it
// runs to the next lone double quote, "" inside stands for one, and a comma or the line's end
// must follow it. An unquoted field may not hold a double quote.
FieldsOrError split_fields(std::string_view line) {
  std::vector<Field> fields;
  std::size_t pos = 0;
  while (true) {
    Field field;
    if (pos < line.size() && line[pos] == '"') {
      field.quoted = true;
      ++pos;
      while (true) {
        if (pos >= line.size()) {
          return std::string("a quoted field is not closed");
        }
        const char c = line[pos++];
        if (c == '"') {
          if (pos < line.size() && line[pos] == '"') {
            field.text += '"';
            ++pos;
            continue;
          }
          break;
        }
        field.text += c;
      }
      if (pos < line.size() && line[pos] != ',') {
        return "the quoted field " + quote(field.text) + " is followed by more text";
      }
    } else {
      const std::size_t comma = line.find(',', pos);
      const std::size_t end = comma == std::string_view::npos ? line.size() : comma;
      const std::string_view token = line.substr(pos, end - pos);
      if (token.find('"') != std::string_view::npos) {
        return quote(token) + " holds a double quote but is not quoted";
      }
      field.text = std::string(token);
      pos = end;
    }
    fields.push_back(std::move(field));
    if (pos >= line.size()) {
      return fields;
    }
    ++pos;  // The comma.
  }
}

// One element, for overlap checks.
using ElementKey = std::pair<ElementKind, std::size_t>;

class Reader {
 public:
  Reader(const Classification& classification, const topology::LinkPlan& plan,
         std::span<const std::string> gateway_names, const time::UtcTime& run_start_utc,
         std::int64_t step_ns)
      : classification_(&classification),
        plan_(&plan),
        gateway_names_(gateway_names),
        run_start_utc_(run_start_utc),
        step_ns_(step_ns),
        step_s_(static_cast<double>(step_ns) / static_cast<double>(kNanosecondsPerSecond)) {
    for (std::size_t i = 0; i < classification.satellites.size(); ++i) {
      index_of_[classification.satellites[i].norad_cat_id] = i;
    }
    for (std::size_t l = 0; l < plan.links.size(); ++l) {
      link_of_.try_emplace(std::minmax(plan.links[l].satellite_a, plan.links[l].satellite_b), l);
    }
    for (std::size_t g = 0; g < gateway_names.size(); ++g) {
      if (!gateway_of_.try_emplace(gateway_names[g], g).second) {
        shared_names_.insert(gateway_names[g]);
      }
    }
  }

  // nullopt if the line is the header or a good failure, else the problem.
  std::optional<std::string> line(std::string_view text) {
    if (!has_header_) {
      if (text == kHeader) {
        has_header_ = true;
        return std::nullopt;
      }
      return "the header is " + quote(text) + ", expected " + std::string(kHeader);
    }
    FieldsOrError split = split_fields(text);
    if (auto* problem = std::get_if<std::string>(&split)) {
      return std::move(*problem);
    }
    const auto& fields = std::get<std::vector<Field>>(split);
    if (fields.size() != kFieldNames.size()) {
      return "has " + std::to_string(fields.size()) + " fields, expected " +
             std::to_string(kFieldNames.size());
    }
    for (std::size_t j = 0; j < fields.size(); ++j) {
      if (fields[j].quoted && j != kGatewayField) {
        return "the " + std::string(kFieldNames.at(j)) +
               " field is quoted; only the gateway field may be";
      }
    }
    const std::optional<ElementKind> parsed_kind = parse_element_kind(fields[0].text);
    if (!parsed_kind) {
      return quote(fields[0].text) + " is not a kind (satellite, laser_link or gateway)";
    }
    const ElementKind kind = *parsed_kind;
    // Which of norad_cat_id_a, norad_cat_id_b and gateway the kind uses.
    std::array<bool, 3> used{};
    switch (kind) {
      case ElementKind::kSatellite:
        used = {true, false, false};
        break;
      case ElementKind::kLaserLink:
        used = {true, true, false};
        break;
      case ElementKind::kGateway:
        used = {false, false, true};
        break;
    }
    for (std::size_t j = 1; j <= 3; ++j) {
      const bool set = !fields[j].text.empty() || fields[j].quoted;
      if (used.at(j - 1) && !set) {
        return "kind " + std::string(name_of(kind)) + " needs " + std::string(kFieldNames.at(j));
      }
      if (!used.at(j - 1) && set) {
        return "kind " + std::string(name_of(kind)) + " must leave " +
               std::string(kFieldNames.at(j)) + " empty";
      }
    }

    std::size_t element = 0;
    switch (kind) {
      case ElementKind::kSatellite: {
        auto satellite = plane_member(fields[1].text);
        if (auto* problem = std::get_if<std::string>(&satellite)) {
          return std::move(*problem);
        }
        element = std::get<std::size_t>(satellite);
        break;
      }
      case ElementKind::kLaserLink: {
        auto link = planned_link(fields[1].text, fields[2].text);
        if (auto* problem = std::get_if<std::string>(&link)) {
          return std::move(*problem);
        }
        element = std::get<std::size_t>(link);
        break;
      }
      case ElementKind::kGateway: {
        const std::string& name = fields[kGatewayField].text;
        if (shared_names_.contains(name)) {
          return "more than one gateway is named " + quote(name);
        }
        const auto it = gateway_of_.find(name);
        if (it == gateway_of_.end()) {
          return "no gateway is named " + quote(name);
        }
        element = it->second;
        break;
      }
    }

    const std::string& start_text = fields[4].text;
    const std::optional<time::CivilTime> civil = time::parse_iso_civil_time(start_text);
    const std::optional<time::UtcTime> start_utc =
        civil ? time::UtcTime::from_civil(*civil) : std::nullopt;
    if (!start_utc) {
      return quote(start_text) +
             " is not a UTC time (YYYY-MM-DDTHH:MM:SS with an optional fraction)";
    }
    const std::optional<std::int64_t> offset = nanoseconds_between(*start_utc, run_start_utc_);
    if (!offset) {
      return "start_utc " + start_text + " is more than 1e18 ns from the run's start";
    }
    const std::int64_t offset_ns = *offset;
    double start_s = 0.0;
    if (offset_ns < 0) {
      start_s = static_cast<double>(offset_ns) / static_cast<double>(kNanosecondsPerSecond);
    } else if (offset_ns % step_ns_ == 0) {
      const std::int64_t step = offset_ns / step_ns_;  // The step k.
      start_s = static_cast<double>(step) * step_s_;
    } else {
      return "start_utc " + start_text + " is neither a step of the run (" +
             format_utc(run_start_utc_) + " + k x " + seconds_text(step_ns_) +
             " s) nor before its start";
    }

    std::optional<double> length_s;
    const std::string& length_text = fields[5].text;
    if (!length_text.empty()) {
      const std::optional<double> parsed_length = parse_decimal(length_text);
      if (!parsed_length) {
        return "length_s " + quote(length_text) + " is not a decimal number";
      }
      const double length = *parsed_length;
      if (length < 0.0) {
        return "length_s " + length_text + " is negative";
      }
      if (length == 0.0) {
        return "length_s " + length_text +
               " is zero: a failure that ends as it starts does nothing";
      }
      if (start_s + length == start_s) {
        return "length_s " + length_text + " is too short to end after the start";
      }
      length_s = length;
    }

    Failure failure{.kind = kind,
                    .element = element,
                    .start_utc = *start_utc,
                    .start_s = start_s,
                    .length_s = length_s};
    std::vector<std::size_t>& earlier = failures_of_[ElementKey{kind, element}];
    for (const std::size_t e : earlier) {
      if (overlap(schedule_.failures[e], failure)) {
        return element_text(kind, element) + " already fails at an overlapping time on line " +
               std::to_string(lines_.at(e));
      }
    }
    earlier.push_back(schedule_.failures.size());
    schedule_.failures.push_back(failure);
    lines_.push_back(line_number_);
    return std::nullopt;
  }

  void set_line_number(std::size_t n) { line_number_ = n; }
  [[nodiscard]] bool has_header() const { return has_header_; }

  FailureSchedule finish() && { return std::move(schedule_); }

 private:
  // The plane member with catalogue number `text`, or why there is none.
  [[nodiscard]] std::variant<std::size_t, std::string> plane_member(std::string_view text) const {
    const std::optional<std::int64_t> id = parse_catalogue_number(text);
    if (!id) {
      return quote(text) + " is not a catalogue number";
    }
    const auto it = index_of_.find(*id);
    if (it == index_of_.end()) {
      return "satellite " + std::to_string(*id) + " is not in the classification";
    }
    const auto& placement = classification_->satellites[it->second].placement;
    if (const auto* reason = std::get_if<constellation::ExclusionReason>(&placement)) {
      return "satellite " + std::to_string(*id) +
             " is in no plane (excluded: " + std::string(constellation::name_of(*reason)) + ")";
    }
    return it->second;
  }

  // The planned link between the satellites `text_a` and `text_b`, or why there is none.
  [[nodiscard]] std::variant<std::size_t, std::string> planned_link(std::string_view text_a,
                                                                    std::string_view text_b) const {
    auto a = plane_member(text_a);
    if (auto* problem = std::get_if<std::string>(&a)) {
      return std::move(*problem);
    }
    auto b = plane_member(text_b);
    if (auto* problem = std::get_if<std::string>(&b)) {
      return std::move(*problem);
    }
    const std::size_t sa = std::get<std::size_t>(a);
    const std::size_t sb = std::get<std::size_t>(b);
    if (sa == sb) {
      return "links satellite " + std::to_string(id(sa)) + " to itself";
    }
    const auto it = link_of_.find(std::minmax(sa, sb));
    if (it == link_of_.end()) {
      return "satellites " + std::to_string(id(sa)) + " and " + std::to_string(id(sb)) +
             " have no link in the laser plan";
    }
    return it->second;
  }

  [[nodiscard]] std::string element_text(ElementKind kind, std::size_t element) const {
    switch (kind) {
      case ElementKind::kSatellite:
        return "satellite " + std::to_string(id(element));
      case ElementKind::kLaserLink: {
        const topology::PlannedLink& link = plan_->links[element];
        return "laser link " + std::to_string(id(link.satellite_a)) + "-" +
               std::to_string(id(link.satellite_b));
      }
      case ElementKind::kGateway:
        return "gateway " + quote(gateway_names_[element]);
    }
    return "?";
  }

  // Whether [start, end) of the two failures meet; a permanent one has no end.
  static bool overlap(const Failure& x, const Failure& y) {
    const std::optional<double> x_end = end_s(x);
    const std::optional<double> y_end = end_s(y);
    return (!y_end || x.start_s < *y_end) && (!x_end || y.start_s < *x_end);
  }

  [[nodiscard]] std::int64_t id(std::size_t satellite) const {
    return classification_->satellites[satellite].norad_cat_id;
  }

  const Classification* classification_;
  const topology::LinkPlan* plan_;
  std::span<const std::string> gateway_names_;
  time::UtcTime run_start_utc_;
  std::int64_t step_ns_;
  double step_s_;
  std::map<std::int64_t, std::size_t> index_of_;
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> link_of_;
  std::map<std::string, std::size_t, std::less<>> gateway_of_;
  std::set<std::string, std::less<>> shared_names_;
  std::map<ElementKey, std::vector<std::size_t>> failures_of_;
  FailureSchedule schedule_;
  std::vector<std::size_t> lines_;  // The line of each failure read.
  bool has_header_ = false;
  std::size_t line_number_ = 0;
};

// The gateway field as written: quoted when the name holds a comma or a double quote.
std::string gateway_field(std::string_view name) {
  if (name.find_first_of(",\"") == std::string_view::npos) {
    return std::string(name);
  }
  std::string text = "\"";
  for (const char c : name) {
    if (c == '"') {
      text += '"';
    }
    text += c;
  }
  text += '"';
  return text;
}

}  // namespace

bool write_failure_schedule(std::ostream& out, const FailureSchedule& schedule,
                            const Classification& classification, const topology::LinkPlan& plan,
                            std::span<const std::string> gateway_names) {
  std::string text = "# satsim failure schedule (docs/phase-4-failures.md 4.2): ";
  text += std::to_string(schedule.failures.size()) + " failures\n";
  text += kHeader;
  text += '\n';
  const auto id = [&](std::size_t satellite) {
    return std::to_string(classification.satellites.at(satellite).norad_cat_id);
  };
  for (const Failure& f : schedule.failures) {
    text += name_of(f.kind);
    switch (f.kind) {
      case ElementKind::kSatellite:
        text += "," + id(f.element) + ",,,";
        break;
      case ElementKind::kLaserLink: {
        const topology::PlannedLink& link = plan.links.at(f.element);
        text += "," + id(link.satellite_a) + "," + id(link.satellite_b) + ",,";
        break;
      }
      case ElementKind::kGateway: {
        const std::string& name = gateway_names[f.element];
        if (name.empty() || name.find_first_of("\r\n") != std::string::npos) {
          return false;
        }
        text += ",,," + gateway_field(name) + ",";
        break;
      }
    }
    text += format_utc(f.start_utc);
    text += ',';
    if (f.length_s) {
      text += double_text(*f.length_s);
    }
    text += '\n';
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  return static_cast<bool>(out);
}

std::optional<std::string> write_failure_schedule_file(const std::filesystem::path& path,
                                                       const FailureSchedule& schedule,
                                                       const Classification& classification,
                                                       const topology::LinkPlan& plan,
                                                       std::span<const std::string> gateway_names) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    return "cannot open " + path.string() + " for writing";
  }
  const bool written = write_failure_schedule(file, schedule, classification, plan, gateway_names);
  file.close();
  if (!written || !file) {
    return "writing " + path.string() + " failed";
  }
  return std::nullopt;
}

FailureScheduleOrError read_failure_schedule(std::istream& in, const Classification& classification,
                                             const topology::LinkPlan& plan,
                                             std::span<const std::string> gateway_names,
                                             const time::UtcTime& run_start_utc,
                                             std::int64_t step_ns) {
  try {
    if (step_ns <= 0) {
      return std::string("the run's step must be positive");
    }
    if (!in) {
      return std::string("input stream is not readable");
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
      return std::string("read error");
    }
    if (text.empty()) {
      return std::string("file is empty");
    }
    Reader reader(classification, plan, gateway_names, run_start_utc, step_ns);
    std::string_view rest = text;
    for (std::size_t line_number = 1; !rest.empty(); ++line_number) {
      const std::size_t newline = rest.find('\n');
      std::string_view line = rest.substr(0, newline);
      rest = newline == std::string_view::npos ? std::string_view() : rest.substr(newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      if (line.starts_with('#')) {
        continue;
      }
      if (line.empty()) {
        return line_error(line_number, "empty line");
      }
      reader.set_line_number(line_number);
      if (auto problem = reader.line(line)) {
        return line_error(line_number, *problem);
      }
    }
    if (!reader.has_header()) {
      return std::string("no header");
    }
    return std::move(reader).finish();
  } catch (const std::exception& e) {
    return std::string("could not load: ") + e.what();
  }
}

FailureScheduleOrError read_failure_schedule_file(const std::filesystem::path& path,
                                                  const Classification& classification,
                                                  const topology::LinkPlan& plan,
                                                  std::span<const std::string> gateway_names,
                                                  const time::UtcTime& run_start_utc,
                                                  std::int64_t step_ns) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    return path.string() + " is a directory";
  }
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return "cannot open " + path.string();
  }
  FailureScheduleOrError result =
      read_failure_schedule(file, classification, plan, gateway_names, run_start_utc, step_ns);
  if (auto* error = std::get_if<std::string>(&result)) {
    *error = path.string() + ": " + *error;
  }
  return result;
}

}  // namespace satsim::failure
