# Phase 4 implementation plan

## Why the plan looks like this

The plan for implementing [the Phase 4 spec](phase-4-failures.md), approved 2026-11-21. The
spec adds unpredicted failures of satellites, laser links
and gateways, three ways of reacting (`planner`, `local-repair`, `flooding`), recovery in
continuous time on each step's frozen network, re-pointing of freed lasers, a new `failures`
program, an independent Python model and a `validate-failures` CI step.

Two explorations of the code found what shapes this plan:

- **Everything assumes one fixed laser plan per run.** `StepLinks::states[i]` is
  `LinkPlan::links[i]` in `LinkStateEvaluator`, `run_physical_checks`, `GraphBuilder::build`
  (`apps/routing/graph.cpp:109`) and `Engine::link_index_`. The cross-plane matcher is a
  private method (`src/topology/link_plan.cpp`, `Planner::match`).
- **The Phase 3 `Engine` reads the network only through `Lookahead`** (`apps/routing/engine.hpp:32`),
  but it is step-based, plans all lanes at once, and `RoutePlanner` has no way to be handed a
  route. Failure causes cannot be named (`DropCause`, `Responsible`).
- `StepSource` computes link states for a look-ahead window in advance, so failures cannot be
  injected through the existing `AfterLinks` seam for the `whole-interval` planner.
- No Python code plans a `three` laser set, and `make_ideal_shell.py` writes only the
  plus-grid link file.

## Design choices that resolve those findings

1. **The failure timeline is worked out before the run steps.** Failures are scheduled, so
   detection instants, report arrivals, repair links (pairs, match step, end of acquisition,
   drops on a return) are all deterministic. A pre-pass in time order produces them, propagating
   positions only at the few steps where lasers are re-matched.
2. **One fixed "run link list" = planned links + every repair link the run will ever have.**
   Each link carries its time-varying status (in force, failed, acquiring, not in force). All
   existing index-by-link code keeps working with a fixed list; nothing grows mid-run.
3. **An overlay turns a step's geometry plus a set of known facts into a `StepGround`.**
   Truth = geometry of step ⌊t⌋ + everything in force at t. A view = the same geometry + only
   the facts that actor knows. Failed satellites are filtered out of the visible lists; failed
   or not-in-force links are marked down. `GraphBuilder` is reused unchanged on overlaid input.
4. **The Phase 3 `Engine` is refactored, not copied:** it takes a truth `Lookahead` and a view
   `Lookahead` (planning and serving choice use the view; the step graph, ideal and held-route
   check use truth), names failure causes, lets a lane's route be replaced (local repair) and
   exposes "evaluate this lane on this graph". With truth = view it is Phase 3 exactly, which is
   how spec 6.2 is met for `planner`. The baseline lane is the same engine with no failures.
5. **`flooding` lanes are new code** on `routing/` tables and a hop-by-hop walk, with trees
   cached per (destination, fact set) and re-evaluated only when a node on the current walk
   changes view.
6. **The independent model gets its own `three` plan:** Python writes a `three` link file for
   Hypatia's shell with its own greedy matching, and the program reads it with `--links-file`
   (four-column form, so laser sides are known for re-matching).

## Units of work

One commit per unit, on branch `phase-4-failures`. Units 1 to 4 touch separate files; the
rest are in order.

| # | Unit | Main files | Done when |
|---|---|---|---|
| 1 | `network`: shortest delays from several sources with a per-edge extra; shortest-path tree *towards* a target; connected pieces (union-find) | `include/satsim/network/`, `src/network/`, `tests/network/` | Hand-worked tests; many-source result equals the lowest of single-source results |
| 2 | `routing`: tables and the hop-by-hop walk with per-node views (delivered, dead end, loop); `RoutePlanner::hold(route)` | `include/satsim/routing/forwarding.hpp`, `planner.hpp`, `tests/routing/` | Hand-worked black hole, loop, delivery; same view everywhere gives the planner's route |
| 3 | `failure/` (new module): schedule types, the failure file (mirror `topology/link_file.cpp`'s `Reader` and exact-message tests), elements down at an instant, who detects what and when | `include/satsim/failure/`, `src/failure/`, `tests/failure/`, root and tests `CMakeLists.txt` | Round trip and every refusal; transient start and end; detectors per element kind |
| 4 | `topology`: re-matching freed lasers (make `Planner::match` reusable on a free-laser set; in-plane bridging across a dead satellite); run link list with statuses; down reasons for failed, acquiring, not in force; the new physical checks of spec 6.1 | `src/topology/link_plan.cpp`, new `link_repair.hpp/.cpp`, `link_state.*`, `physical_checks.*`, `tests/topology/` | Synthetic shell with known freed lasers and partners; return restores the original; each new check made to fire. **Existing programs' output stays byte-identical** |
| 5 | `--plan-time` in `apps/inputs` (member added to every `Options` that goes through `input_options_of`; Earth-orientation check covers it). `Engine` refactor per choice 4; three failure drop causes | `apps/inputs/`, `apps/routing/engine.*`, `settings.*`, `run.cpp`, `tests/routing_app/` | All Phase 3 tests, `validate-routing` and `RoutingRealDataCheck` green; a short real-data `routing` run gives a byte-identical CSV and summary before and after |
| 6 | `apps/failures` part 1: timeline pre-pass, overlay, control graph (satellites, dishes, gateways) for report times, baseline + `planner` + `local-repair` lanes, intra-step events, events and facts CSVs, measures of 4.7, checks of 6.1, summary, `--replan-on-report`, `--repoint` | `apps/failures/`, `tests/failures_app/` (on `tests/routing_app/fake.*` and `tests/topology_app/fixture.*`) | Every interval hand-worked on a synthetic shell for a satellite, a link, a gateway; stale plan picks a dead element; backup that is itself dead; old failures known at step 0; empty schedule equals `routing` (check executable, both models, real snapshot) |
| 7 | `apps/failures` part 2: `flooding` lanes (flood times, tables, walks, messages, dish re-pick), after-recovery measures (reachability, largest piece) | `apps/failures/`, `tests/failures_app/` | Hand-worked flood and transition on a synthetic shell; settled route equals the planner's; no-failure `flooding` equals `lowest-delay` rows |
| 8 | Independent model and CI: `three` link file for the ideal shell; `python/validate_failures.py` (reuse positions, cities and Dijkstra pieces of `validate_routing.py`; own flood, reports, three modes, re-matching, union-find); `validate-failures` step | `python/`, `scripts/ci.sh`, `scripts/check.sh`, `.github/workflows/ci.yml`, `docs/ci.md`, `CLAUDE.md` | Program and model agree exactly on every item of spec 6.3; planted C++ errors make the step exit 1 |
| 9 | Runs: `python/make_failures.py` (scenarios from the baseline CSV, seeded random shares, attack ranking), `python/run_phase4.py` (the groups of spec 5, recount cross-checked against the program's summary as `run_phase3.py` does) | `python/`, `docs/phase-4-data/` | Trial run clean; then the full runs, every run exit 0 with zero failed checks |
| 10 | Report: `python/plot_phase4.py`, `docs/phase-4-results.md` (incl. the StarGlider 7% and percolation comparisons, no pass or fail), README, spec status line | `python/`, `docs/` | Every number from committed data |

## Rules that apply to every unit

- Check: the 9 steps (10 after unit 8) via `scripts/ci.sh` in the `satsim-ci` image (cloud
  workaround in CLAUDE.md); `asan-clang` while iterating.
- Planted bugs, one at a time, never committed, each failing a named test.
- Sanitizer tests on small shells only (`tests/support/synthetic.hpp`, `tests/topology/ideal.*`);
  anything on the full snapshot is a check executable built without sanitizers.
- Module order kept: `routing` depends only on `network`; `failure` on `topology` and earlier.
- No edit to `DECISIONS.md` or to a spec rule. If the code cannot do what the spec says, or a
  result contradicts a decision, that unit stops and the owner is asked.
- Push the branch after every commit. No merge to `main` and no pull request without the
  owner's OK.

## Risks, and the guard for each

- **Unit 5 touches validated Phase 3 code.** Guard: byte-identical `routing` output on a real
  run, plus `validate-routing`, before anything else builds on it.
- **Unit 4 changes shared enums and checks.** Guard: `topology` and `routing` summaries and
  CSVs unchanged (their tests, `run_phase2.py` and `run_phase3.py` parse them).
- **Spec 4.5 and 6.2 can disagree on an exact tie:** tables break ties from the destination's
  side, the planner from the source's. On real geometry exact ties should not occur; on an
  ideal shell they might. If the empty-schedule comparison shows one, it is reported rather than
  either rule bent.
- **`flooding` cost on `plane` and `region` scenarios** (hundreds of facts). Trees are cached
  per fact set and walks re-evaluated only when a node on the walk changes view; if a run is
  still too slow the measured time is reported before any rule changes.
- **The realistic model and gateway loss are outside the independent model** (as the spec
  says); they rest on unit 6's tests and the empty-schedule check.

## Verification, end to end

- `scripts/ci.sh all` green in the image, including `validate-failures`.
- `failures` with no `--failures` file against `routing` on the real snapshot: identical rows,
  both models.
- `python3 python/run_phase4.py --failures build/validate/bin/failures ...`: every run exit 0,
  zero failed checks, zero differences between the Python recount and the program's summary.
- CI green on the branch's pull request once the owner asks for one.
