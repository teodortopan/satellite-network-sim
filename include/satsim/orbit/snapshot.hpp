#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/omm_loader.hpp"
#include "satsim/orbit/propagator.hpp"

namespace satsim::orbit {

// A snapshot is one directory written by python/fetch_elements.py (data/starlink/<date>/). These
// functions load it for any program that needs it; each step is separate so a program can check
// what is quick (the directory, the Earth-orientation file) before parsing the element files.

/// The files of a snapshot directory.
inline constexpr std::string_view kStandardElementsFileName = "gp.json";
inline constexpr std::string_view kSupplementalElementsFileName = "supgp.json";
inline constexpr std::string_view kEarthOrientationFileName = "finals2000A.all";

/// Checks that `dir` exists, is a directory, and holds every file in `file_names` as a regular
/// file. Returns nullopt if so, else a message naming the directory and every missing file.
[[nodiscard]] std::optional<std::string> check_snapshot_directory(
    const std::filesystem::path& dir, std::span<const std::string_view> file_names);

/// What loading one OMM JSON file gave: the number of good records and every bad one.
struct ElementFileLoad {
  std::size_t records_loaded = 0;
  std::vector<RecordError> record_errors;
};

/// A snapshot's element sets: both files loaded and one element set chosen per satellite
/// (select_element_sets: supplemental where it exists, otherwise standard).
struct SnapshotElements {
  ElementFileLoad standard_file;
  ElementFileLoad supplemental_file;
  ElementSelection selection;
};

/// Snapshot elements, or why the files could not be used.
using SnapshotElementsOrError = std::variant<SnapshotElements, std::string>;

/// Loads gp.json, then supgp.json, from `dir` and selects one element set per satellite.
/// Fails, with the loader's message (which names the path), on the first file that cannot be
/// used as a whole; bad records are only counted. Does not check the directory itself (see
/// check_snapshot_directory).
[[nodiscard]] SnapshotElementsOrError load_snapshot_elements(const std::filesystem::path& dir);

/// One satellite: its chosen element set and the propagator built from it.
struct SnapshotSatellite {
  SourcedElementSet element_set;
  Propagator propagator;
};

/// A satellite whose propagator could not be built, and why.
struct PropagatorBuildFailure {
  SourcedElementSet element_set;
  PropagationError error = PropagationError::kInvalidElements;
};

/// The outcome of build_propagators: every input lands in exactly one of the two lists, each
/// in input order.
struct BuiltPropagators {
  std::vector<SnapshotSatellite> satellites;
  std::vector<PropagatorBuildFailure> failures;
};

/// Builds a propagator for each element set (Propagator::create).
[[nodiscard]] BuiltPropagators build_propagators(std::vector<SourcedElementSet> element_sets);

}  // namespace satsim::orbit
