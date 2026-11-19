#pragma once

// Fixtures of the topology program's tests: a synthetic snapshot (an ideal Walker shell that
// the default classification puts in its 53deg shell) written to a scratch directory, and the
// small ground-data directory in tests/topology_app/data.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "satsim/orbit/element_set.hpp"
#include "support/synthetic.hpp"
#include "topology/arguments.hpp"

namespace satsim::topology_app::testing {

/// tests/topology_app/data/ (tests run from the repository root).
inline const std::filesystem::path kData = "tests/topology_app/data";
/// gateways.json (two operational, one planned), pop_codes.json, starlink_pops.csv and
/// starlink_geofeed.csv; no assumed_gateways.json.
inline const std::filesystem::path kGroundDir = kData / "ground";
/// One operational assumed gateway, to copy next to the others.
inline const std::filesystem::path kAssumedGateways = kData / "assumed_gateways.json";
/// Four cities (Alpha, Beta, Gamma without service, Delta) and three pairs (two study, one
/// validation).
inline const std::filesystem::path kCities = kData / "cities.json";

/// 10 planes of 16 satellites at 53.16 deg, at the height the default classification's 53deg
/// shell expects (its mean geodetic height is in 467.5 to 471 km), epoch 2026-11-18T12:00.
[[nodiscard]] constellation::testing::WalkerShell fixture_shell();

/// The start of the fixture runs: the shell's epoch.
inline constexpr std::string_view kStart = "2026-11-18T12:00:00";

/// Writes `sets` as gp.json (OMM JSON), an empty supgp.json and the propagate fixture's
/// finals2000A.all (2026-10-01 to 2026-12-31) into `dir`.
void write_snapshot(const std::filesystem::path& dir, std::span<const orbit::ElementSet> sets);

/// Copies the ground fixture into `dir` (which must exist).
void copy_ground(const std::filesystem::path& dir);

/// Options for a fixture run: the given snapshot directory, the ground fixture, the cities
/// fixture, `duration_s` every `step_s` from kStart, output to standard output.
[[nodiscard]] Options fixture_options(const std::filesystem::path& snapshot_dir,
                                      std::int64_t duration_s, std::int64_t step_s,
                                      topology::LaserPattern pattern, EndpointModel model);

}  // namespace satsim::topology_app::testing
