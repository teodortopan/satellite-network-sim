#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "inputs/inputs.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/positions.hpp"

// Steps a run through time: each step's positions, laser link states and ground links, for
// the programs that share the inputs (inputs.hpp). Optionally keeps a sliding window of future
// steps for programs that predict (Phase 3 spec 4.3 to 4.5), so that every step is propagated
// once however far ahead it is first needed.

namespace satsim::app {

/// Called on a step's positions and link states as soon as the link states are computed, and
/// before its ground links: a test seam (never set by the programs).
using AfterLinks = std::function<void(topology::StepPositions&, topology::StepLinks&)>;

/// The sliding window. Steps are numbered from 0 at the run's start, start + k × step; a
/// window may run past the run's last step (prepare_inputs' look-ahead must then cover it).
///
/// - positions(j) for j < position_window: the positions j steps after the current one.
/// - links(j) and visible(j) for j < full_window: that step's laser link states
///   (topology::LinkStateEvaluator, called once per step in time order) and ground links
///   (GroundVisibility::at, sites: every city of the inputs, then every gateway).
///
/// full_window <= position_window (the constructor raises position_window to it), both >= 1.
class StepSource {
 public:
  /// `inputs` must outlive the source. The run's start, step, limits and mask come from
  /// `options`.
  StepSource(const Inputs& inputs, const InputOptions& options, std::size_t position_window,
             std::size_t full_window, AfterLinks after_links = {});

  /// Moves to the next step (the first call moves to step 0) and fills the window. Returns an
  /// error naming the time if a step has no Earth-orientation values.
  [[nodiscard]] std::optional<std::string> advance();

  /// The current step's number; -1 before the first advance.
  [[nodiscard]] std::int64_t index() const noexcept { return index_; }

  [[nodiscard]] std::size_t position_window() const noexcept { return position_window_; }
  [[nodiscard]] std::size_t full_window() const noexcept { return full_window_; }

  [[nodiscard]] const topology::StepPositions& positions(std::size_t offset = 0) const {
    return positions_.at(offset);
  }
  /// The positions window, from the current step: steps[k] are k steps ahead.
  [[nodiscard]] std::span<const topology::StepPositions> position_steps() const noexcept {
    return positions_;
  }
  [[nodiscard]] const topology::StepLinks& links(std::size_t offset = 0) const {
    return full_.at(offset).links;
  }
  [[nodiscard]] const std::vector<std::vector<topology::VisibleSatellite>>& visible(
      std::size_t offset = 0) const {
    return full_.at(offset).visible;
  }

  [[nodiscard]] const topology::GroundVisibility& visibility() const noexcept {
    return visibility_;
  }
  /// Every city, then every gateway.
  [[nodiscard]] std::span<const ground::GroundSite> sites() const noexcept { return sites_; }
  [[nodiscard]] std::size_t city_count() const noexcept { return city_count_; }

 private:
  struct Full {
    topology::StepLinks links;
    std::vector<std::vector<topology::VisibleSatellite>> visible;
  };

  [[nodiscard]] std::optional<std::string> push_positions(std::int64_t step);
  void push_full(std::size_t offset);

  const Inputs* inputs_;
  time::UtcTime start_;
  std::int64_t step_ns_;
  std::size_t position_window_;
  std::size_t full_window_;
  AfterLinks after_links_;
  std::vector<ground::GroundSite> sites_;
  std::size_t city_count_;
  topology::GroundVisibility visibility_;
  topology::LinkStateEvaluator link_states_;
  std::int64_t index_ = -1;
  /// positions_[k]: step index_ + k.
  std::vector<topology::StepPositions> positions_;
  /// full_[k]: step index_ + k.
  std::vector<Full> full_;
};

}  // namespace satsim::app
