#include "satsim/topology/link_file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::topology {

namespace {

using constellation::Classification;
using constellation::PlaneAssignment;

constexpr std::string_view kShortHeader = "norad_cat_id_a,norad_cat_id_b";
constexpr std::string_view kLongHeader = "norad_cat_id_a,norad_cat_id_b,side_a,side_b";
constexpr std::size_t kMaxQuotedLength = 40;  // Keeps messages short on junk input.

constexpr std::array<LaserSide, 6> kAllLaserSides = {
    LaserSide::kAhead, LaserSide::kBehind,     LaserSide::kEast,
    LaserSide::kWest,  LaserSide::kSecondEast, LaserSide::kSecondWest};

std::string quote(std::string_view text) {
  if (text.size() > kMaxQuotedLength) {
    return "\"" + std::string(text.substr(0, kMaxQuotedLength)) + "...\"";
  }
  return "\"" + std::string(text) + "\"";
}

std::vector<std::string_view> split(std::string_view line) {
  std::vector<std::string_view> fields;
  while (true) {
    const std::size_t at = line.find(',');
    if (at == std::string_view::npos) {
      fields.push_back(line);
      return fields;
    }
    fields.push_back(line.substr(0, at));
    line.remove_prefix(at + 1);
  }
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

std::optional<LaserSide> parse_side(std::string_view text) {
  for (const LaserSide side : kAllLaserSides) {
    if (text == name_of(side)) {
      return side;
    }
  }
  return std::nullopt;
}

// The kind of a link whose ends use side_a and side_b, or nullopt if they are not a pair.
std::optional<LinkKind> kind_of(LaserSide side_a, LaserSide side_b) {
  if (side_a == LaserSide::kAhead && side_b == LaserSide::kBehind) {
    return LinkKind::kInPlane;
  }
  if (side_a == LaserSide::kEast && side_b == LaserSide::kWest) {
    return LinkKind::kCrossPlane;
  }
  if (side_a == LaserSide::kSecondEast && side_b == LaserSide::kSecondWest) {
    return LinkKind::kSecondCrossPlane;
  }
  return std::nullopt;
}

std::string line_error(std::size_t line, const std::string& message) {
  return "line " + std::to_string(line) + ": " + message;
}

// Why an unlinked laser at `side` of a satellite in a plane of `plane_size` with neighbours `n`
// has no link: the planner's reasons.
UnassignedReason unassigned_reason(LaserSide side, std::size_t plane_size,
                                   const PlaneNeighbours& n) {
  std::optional<std::size_t> neighbour;
  switch (side) {
    case LaserSide::kAhead:
    case LaserSide::kBehind:
      return plane_size <= 2 ? UnassignedReason::kPlaneTooSmall : UnassignedReason::kNoFreePartner;
    case LaserSide::kEast:
      neighbour = n.east;
      break;
    case LaserSide::kWest:
      neighbour = n.west;
      break;
    case LaserSide::kSecondEast:
      neighbour = n.second_east;
      break;
    case LaserSide::kSecondWest:
      neighbour = n.second_west;
      break;
  }
  return neighbour ? UnassignedReason::kNoFreePartner : UnassignedReason::kNoNeighbourPlane;
}

// The plan's per-satellite tables from its links (see the header).
void fill_lasers(LinkPlan& plan, const Classification& classification) {
  const std::size_t count = classification.satellites.size();
  plan.lasers.assign(count, {});
  for (std::size_t i = 0; i < count; ++i) {
    if (const auto* place = std::get_if<PlaneAssignment>(&classification.satellites[i].placement)) {
      for (const LaserSide side : laser_sides(plan.pattern, place->index_in_plane)) {
        plan.lasers[i].push_back(Laser{.side = side, .link = std::nullopt, .unassigned = {}});
      }
    }
  }
  for (std::size_t l = 0; l < plan.links.size(); ++l) {
    const PlannedLink& link = plan.links[l];
    for (const auto& [end, side] :
         {std::pair{link.satellite_a, link.side_a}, std::pair{link.satellite_b, link.side_b}}) {
      std::vector<Laser>& lasers = plan.lasers[end];
      const auto free = std::ranges::find_if(
          lasers, [&](const Laser& laser) { return laser.side == side && !laser.link; });
      if (free != lasers.end()) {
        free->link = l;
      } else {
        lasers.push_back(Laser{.side = side, .link = l, .unassigned = {}});
      }
    }
  }
  plan.neighbours.clear();
  for (const auto& planes : classification.planes) {
    plan.neighbours.push_back(plane_neighbours(planes));
  }
  for (std::size_t i = 0; i < count; ++i) {
    const constellation::ClassifiedSatellite& satellite = classification.satellites[i];
    const auto* place = std::get_if<PlaneAssignment>(&satellite.placement);
    if (place == nullptr || !satellite.shell_index) {
      continue;
    }
    const std::size_t shell = *satellite.shell_index;
    const std::size_t plane_size = classification.planes[shell][place->plane_index].members.size();
    const PlaneNeighbours& n = plan.neighbours[shell][place->plane_index];
    for (Laser& laser : plan.lasers[i]) {
      if (!laser.link) {
        laser.unassigned = unassigned_reason(laser.side, plane_size, n);
      }
    }
  }
}

// The reader's state: the satellites by catalogue number and what has been read so far.
class Reader {
 public:
  Reader(const Classification& classification, const LinkFileSettings& settings)
      : classification_(&classification),
        limit_(settings.max_lasers.value_or(laser_budget(settings.pattern))),
        links_per_satellite_(classification.satellites.size(), 0) {
    plan_.pattern = settings.pattern;
    if (limit_ != laser_budget(settings.pattern)) {
      plan_.max_lasers = limit_;
    }
    for (std::size_t i = 0; i < classification.satellites.size(); ++i) {
      index_of_[classification.satellites[i].norad_cat_id] = i;
    }
  }

  // nullopt if the line is the header or a good link, else the problem.
  std::optional<std::string> line(std::string_view text) {
    if (!sides_.has_value()) {
      if (text == kShortHeader || text == kLongHeader) {
        sides_ = text == kLongHeader;
        return std::nullopt;
      }
      return "the header is " + quote(text) + ", expected " + std::string(kShortHeader) + " or " +
             std::string(kLongHeader);
    }
    const std::vector<std::string_view> fields = split(text);
    const std::size_t expected = *sides_ ? 4 : 2;
    if (fields.size() != expected) {
      return "has " + std::to_string(fields.size()) + " fields, expected " +
             std::to_string(expected);
    }
    std::array<std::size_t, 2> ends{};
    for (std::size_t k = 0; k < 2; ++k) {
      const std::optional<std::int64_t> id = parse_catalogue_number(fields[k]);
      if (!id) {
        return quote(fields[k]) + " is not a catalogue number";
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
      ends.at(k) = it->second;
    }
    const auto [a, b] = ends;
    PlannedLink link{.satellite_a = a,
                     .satellite_b = b,
                     .side_a = LaserSide::kEast,
                     .side_b = LaserSide::kWest,
                     .kind = LinkKind::kCrossPlane,
                     .shell_index = shell_of(a)};
    if (*sides_) {
      const std::optional<LaserSide> side_a = parse_side(fields[2]);
      const std::optional<LaserSide> side_b = parse_side(fields[3]);
      if (!side_a || !side_b) {
        return quote(side_a ? fields[3] : fields[2]) + " is not a laser side";
      }
      const std::optional<LinkKind> kind = kind_of(*side_a, *side_b);
      if (!kind) {
        return "sides " + std::string(fields[2]) + " and " + std::string(fields[3]) +
               " are not ahead/behind, east/west or second_east/second_west";
      }
      link.side_a = *side_a;
      link.side_b = *side_b;
      link.kind = *kind;
    } else if (same_plane(a, b)) {
      link.side_a = LaserSide::kAhead;
      link.side_b = LaserSide::kBehind;
      link.kind = LinkKind::kInPlane;
    }
    if (a == b) {
      return "links satellite " + std::to_string(id(a)) + " to itself";
    }
    const auto [seen, inserted] = lines_of_pairs_.try_emplace(std::minmax(a, b), line_number_);
    if (!inserted) {
      return "links " + std::to_string(id(a)) + " and " + std::to_string(id(b)) +
             ", already linked on line " + std::to_string(seen->second);
    }
    for (const std::size_t end : {a, b}) {
      if (++links_per_satellite_[end] > limit_) {
        return "gives satellite " + std::to_string(id(end)) + " more than " +
               std::to_string(limit_) + " links";
      }
    }
    if (shell_of(a) != shell_of(b)) {
      plan_.cross_shell_links = true;
    }
    plan_.links.push_back(link);
    return std::nullopt;
  }

  void set_line_number(std::size_t n) { line_number_ = n; }
  [[nodiscard]] bool has_header() const { return sides_.has_value(); }

  LinkPlan finish() && {
    fill_lasers(plan_, *classification_);
    return std::move(plan_);
  }

  [[nodiscard]] bool empty() const { return plan_.links.empty(); }

 private:
  [[nodiscard]] std::int64_t id(std::size_t satellite) const {
    return classification_->satellites[satellite].norad_cat_id;
  }
  [[nodiscard]] std::size_t shell_of(std::size_t satellite) const {
    return classification_->satellites[satellite].shell_index.value_or(0);
  }
  [[nodiscard]] bool same_plane(std::size_t a, std::size_t b) const {
    const auto& pa = std::get<PlaneAssignment>(classification_->satellites[a].placement);
    const auto& pb = std::get<PlaneAssignment>(classification_->satellites[b].placement);
    return shell_of(a) == shell_of(b) && pa.plane_index == pb.plane_index;
  }

  const Classification* classification_;
  std::size_t limit_;
  LinkPlan plan_;
  std::map<std::int64_t, std::size_t> index_of_;
  std::vector<std::size_t> links_per_satellite_;
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> lines_of_pairs_;
  std::optional<bool> sides_;  // Set once the header is read: whether it has the side columns.
  std::size_t line_number_ = 0;
};

}  // namespace

bool write_link_plan(const LinkPlan& plan, const Classification& classification,
                     std::ostream& out) {
  std::string text = "# satsim laser link plan (docs/phase-3-routing.md 4.9): pattern ";
  text += name_of(plan.pattern);
  text += ", " + std::to_string(plan.links.size()) + " links\n";
  text += "# one link per line: the two satellites' catalogue numbers and the laser side at each\n";
  text += kLongHeader;
  text += '\n';
  for (const PlannedLink& link : plan.links) {
    text += std::to_string(classification.satellites.at(link.satellite_a).norad_cat_id);
    text += ',';
    text += std::to_string(classification.satellites.at(link.satellite_b).norad_cat_id);
    text += ',';
    text += name_of(link.side_a);
    text += ',';
    text += name_of(link.side_b);
    text += '\n';
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  return static_cast<bool>(out);
}

std::optional<std::string> write_link_plan_file(const LinkPlan& plan,
                                                const Classification& classification,
                                                const std::filesystem::path& path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    return "cannot open " + path.string() + " for writing";
  }
  const bool written = write_link_plan(plan, classification, file);
  file.close();
  if (!written || !file) {
    return "writing " + path.string() + " failed";
  }
  return std::nullopt;
}

LinkPlanOrError read_link_plan(std::istream& in, const Classification& classification,
                               const LinkFileSettings& settings) {
  try {
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
    Reader reader(classification, settings);
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
      return std::string("no header and no links");
    }
    if (reader.empty()) {
      return std::string("no links");
    }
    return std::move(reader).finish();
  } catch (const std::exception& e) {
    return std::string("could not load: ") + e.what();
  }
}

LinkPlanOrError read_link_plan_file(const std::filesystem::path& path,
                                    const Classification& classification,
                                    const LinkFileSettings& settings) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    return path.string() + " is a directory";
  }
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return "cannot open " + path.string();
  }
  LinkPlanOrError result = read_link_plan(file, classification, settings);
  if (auto* error = std::get_if<std::string>(&result)) {
    *error = path.string() + ": " + *error;
  }
  return result;
}

}  // namespace satsim::topology
