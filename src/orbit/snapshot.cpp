#include "satsim/orbit/snapshot.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/omm_loader.hpp"
#include "satsim/orbit/propagator.hpp"

namespace satsim::orbit {

namespace {

ElementFileLoad summarise_load(const LoadResult& load) {
  return {.records_loaded = load.element_sets.size(), .record_errors = load.record_errors};
}

}  // namespace

std::optional<std::string> check_snapshot_directory(const std::filesystem::path& dir,
                                                    std::span<const std::string_view> file_names) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return "snapshot directory " + dir.string() + " does not exist";
  }
  if (!fs::is_directory(dir, ec)) {
    return "snapshot " + dir.string() + " is not a directory";
  }
  std::string missing;
  for (const std::string_view name : file_names) {
    if (!fs::is_regular_file(dir / name, ec)) {
      missing += (missing.empty() ? "" : ", ") + std::string(name);
    }
  }
  if (!missing.empty()) {
    return "snapshot directory " + dir.string() + " has no " + missing;
  }
  return std::nullopt;
}

SnapshotElementsOrError load_snapshot_elements(const std::filesystem::path& dir) {
  LoadResult standard = load_omm_json(dir / kStandardElementsFileName);
  if (standard.file_error) {
    return *standard.file_error;  // Already names the path.
  }
  LoadResult supplemental = load_omm_json(dir / kSupplementalElementsFileName);
  if (supplemental.file_error) {
    return *supplemental.file_error;
  }
  SnapshotElements elements{
      .standard_file = summarise_load(standard),
      .supplemental_file = summarise_load(supplemental),
      .selection = select_element_sets(standard.element_sets, supplemental.element_sets),
  };
  return elements;
}

BuiltPropagators build_propagators(std::vector<SourcedElementSet> element_sets) {
  BuiltPropagators built;
  for (SourcedElementSet& chosen : element_sets) {
    PropagatorOrError result = Propagator::create(chosen.elements);
    if (const auto* error = std::get_if<PropagationError>(&result)) {
      built.failures.push_back({.element_set = std::move(chosen), .error = *error});
      continue;
    }
    built.satellites.push_back(
        {.element_set = std::move(chosen), .propagator = std::move(std::get<Propagator>(result))});
  }
  return built;
}

}  // namespace satsim::orbit
