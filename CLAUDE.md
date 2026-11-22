# satellite-network-sim

A simulator of a Starlink-like satellite network on real public orbital data.

On the owner's computer, global rules (workflow, commits, review, self-review)
load from `~/.claude/CLAUDE.md`. A cloud session never sees that file, so the
ones this project relies on are repeated under "Working rules" below.
Project-specific rules follow. The Phase 1 spec (`docs/phase-1-orbits.md`) has a
glossary of every orbital term used here.

## Stack

- **C++ (C++20, CMake, GoogleTest): the engine.** Orbit propagation, frames
  and time, topology, routing, failure simulation. All performance-sensitive
  work lives here.
- **TypeScript: the visual dashboard.**
- **Python: analysis, plots and validation.** Never the performance layer.

## Commands

- **`scripts/check.sh` is the single check command.** It builds the Docker
  image in `docker/Dockerfile` and runs every CI step inside it: C++ format,
  Python lint, ASan + UBSan tests with GCC and with Clang, Valgrind,
  clang-tidy, `validate` (the `propagate` program against Skyfield on the
  committed snapshot), `validate-topology` (the `topology` program against an
  independent Python model on Hypatia's ideal shell), `validate-routing` (the
  `routing` program against an independent Python model on Hypatia's and the
  Bariloche paper's shells, `python/validate_routing.py`) and `validate-failures`
  (the `failures` program against an independent Python model on Hypatia's shell with
  a +Grid and a `three` plan, `python/validate_failures.py`). Exit 0 only if all pass.
  It needs only Docker on the host.
- `scripts/check.sh <step>` runs one step (`format`, `python-lint`,
  `asan-gcc`, `asan-clang`, `valgrind`, `tidy`, `validate`, `validate-topology`,
  `validate-routing`, `validate-failures`).
  Measured in a cloud session (x86-64, 4 cores, 2026-11-21): the full check takes
  about 7 minutes once the build trees exist, and longer on a session's first run,
  which compiles everything. clang-tidy is the longest step (about 8 minutes when
  the cores are shared with other work); the 805 sanitizer tests take under a minute
  with either compiler. Prefer `asan-clang` while iterating.
- `python/compare_spacex.py` (against SpaceX's trajectory files) and
  `python/plot_phase1.py` (the report's plots) are run by hand; the exact
  commands are in `docs/phase-1-results.md`. The same goes for Phase 2:
  `python/run_phase2.py` (the 12-hour runs and their summaries) and
  `python/plot_phase2.py`, with commands in `docs/phase-2-results.md`.
  Phase 3's published reproductions (`python/reproduce_routing.py hypatia|bariloche`)
  and the LENS comparison (`python/compare_lens.py`) are run by hand too; usage in
  each script's docstring.
  So are Phase 3's 15 real-data runs (`python/run_phase3.py`, about 30 minutes) and
  `python/plot_phase3.py`, with commands in `docs/phase-3-results.md`.
  Phase 4's runs (`python/run_phase4.py`, about 3,300 short runs, about two hours on 4 cores; `--reuse`
  picks up where a stopped run left off; failure files from `python/make_failures.py`), its
  two comparisons (`python/compare_failures.py`) and `python/plot_phase4.py` are run by hand
  as well, with commands in `docs/phase-4-results.md`.
- `python/fetch_ground.py` downloads the ground data and
  `python/make_assumed_gateways.py` derives the assumed gateways; both by hand.
  `python/fetch_lens.py` downloads the LENS ping files (one request each, no
  retries; the files stay out of git, `data/lens/<date>/metadata.json` is kept).
- CI (`.github/workflows/ci.yml`) runs the same steps through the same
  `scripts/ci.sh`, so a green local check means a green CI. Prefer adding a
  step to `scripts/ci.sh` over putting logic in the workflow file, because
  logic that lives only in the workflow cannot be run locally.
- What each step catches and why: `docs/ci.md`.

## Architecture

| Directory | Module |
|---|---|
| `include/satsim/core/`, `src/core/` | Angle conversions |
| `include/satsim/time/`, `src/time/` | `UtcTime` (two-part Julian date), GMST, UT1 and TT, the IERS Earth-orientation table |
| `include/satsim/frames/`, `src/frames/` | TEME to ITRF, TEME to J2000 (IAU-76/FK5), WGS-84 geodetic |
| `include/satsim/orbit/`, `src/orbit/` | OMM JSON loader, element-set choice, the SGP4 wrapper |
| `third_party/sgp4/` | Vallado's SGP4, vendored unmodified from python-sgp4 |
| `include/satsim/constellation/`, `src/constellation/` | Shells, planes, order in a plane, exclusion reasons, shell definitions from a file |
| `include/satsim/ground/`, `src/ground/` | Cities, gateways, PoPs, the home-PoP rule, great-circle distance, fibre delay |
| `include/satsim/topology/`, `src/topology/` | Positions per step, line of sight, elevation and masks, laser patterns, link state, ground links, physical checks |
| `include/satsim/failure/`, `src/failure/` | The failure schedule and its file, what is down at an instant, who can detect a failure from the laser plan |
| `include/satsim/network/`, `src/network/` | A weighted graph, Dijkstra (also avoiding given nodes), whether a path is up and its delay |
| `include/satsim/routing/`, `src/routing/` | The four routing strategies, the held route of one direction, how a route change is classified |
| `apps/propagate/` | The `propagate` program: snapshot and time range in, CSV out |
| `apps/constellation/` | The `constellation` program: each satellite's shell, plane and position, or why it has none |
| `apps/topology/` | The `topology` program: delay per city pair, direction and time step, under two endpoint models |
| `apps/routing/` | The `routing` program: each direction's route over time under several strategies, with the cause of every drop |
| `apps/failures/` | The `failures` program: what a failure schedule causes over time, the truth and each actor's view, every lane (baseline, reference, planner, local repair, flooding) stepped through it in continuous time, its measures, checks and CSVs |
| `apps/inputs/` | What `topology` and `routing` share: loading the snapshot and ground data, city pairs, positions and link states per step |
| `apps/common/` | Helpers every program shares: CSV, statistics, time steps, arguments |
| `tests/<module>/` | GoogleTest per module, with small fixtures |
| `tests/failures_app/` | The `failures` program's core on hand-made geometry, re-matching on ideal shells, the real geometry against `StepSource` |
| `python/` | Snapshot and ground-data download, reference generation, validation (four independent models), SpaceX comparison, the Phase 2 runs, the Phase 3 reproductions, the Phase 4 failure files, runs and comparisons, plots |
| `data/starlink/<date>/` | One dated snapshot: element sets, IERS file, trajectory checksums |
| `data/ground/` | Cities and pairs; per date: SpaceX's PoP and location files, the community gateway list, assumed gateways; `hypatia/`: Hypatia's 100 cities |
| `data/lens/<date>/` | Checksums of the LENS dish ping files (the files are not in git) |
| `docs/` | Per phase: the spec (`phase-N-<name>.md`), the report (`phase-N-results.md`), its data and plots; `ci.md` |

Later modules may depend on earlier ones (core, time, frames, orbit,
constellation, ground, topology), never the reverse. `failure` may depend on
`topology` and the modules before it. `network` depends on nothing else, and
`routing` only on `network`.

## Conventions

- Prefer kilometres, seconds and radians inside the engine, with the unit in
  the name (`altitude_km`, `gmst_rad`), over degrees or metres, because a
  unit that is not in the name gets mixed silently. Degrees only at input and
  output.
- Prefer a frame in the type or function name (`teme_to_ecef`,
  `position_ecef_km`) over a bare vector, because positions in different
  frames look identical and mixing them gives plausible wrong numbers.
- Prefer a time scale in the type or parameter name (`utc`, `ut1`, `tt`)
  over a bare instant, for the same reason.
- Prefer returning a failed propagation as an error value that is counted
  over a position, because a decayed satellite must never look like a real
  one.
- Prefer `Eigen::Vector3d` for vectors over arrays or structs.

## Non-obvious domain rules

- Prefer feeding GP element sets only to SGP4 over any textbook orbit
  formula, because they are mean elements fitted to SGP4 and mean nothing
  outside it (errors of many kilometres).
- Prefer WGS-72 constants inside SGP4 and WGS-84 for latitude, longitude and
  height over one ellipsoid for both, because the element sets were fitted
  with WGS-72 and maps use WGS-84. Swapping either is a mistake (WGS-84 in
  SGP4 moves positions by 50 m to 6 km over three days).
- Prefer naming the frame at every step of TEME (SGP4 output, quasi-inertial)
  to PEF (rotated by GMST) to ITRF (plus polar motion) and TEME to J2000
  over treating them as interchangeable, because mixing them is silent: it
  produces positions that look right and are off by metres to thousands of
  kilometres.
- Prefer a time as whole days plus a fraction (or integer nanoseconds) over
  one `double` Julian date, because one double resolves only about 40 us,
  30 cm along the orbit. In Python, build Skyfield times with `ts.utc(...)`
  fields, never one float JD.
- Prefer UT1 for GMST and TT for precession and nutation over UTC for both,
  because GMST follows the Earth's actual rotation (ignoring UT1 - UTC costs
  about 36 m) and precession and nutation run on uniform time.
- Prefer keeping the catalogue number in this project's types over passing it
  into the vendored SGP4, because SGP4 copies it into a six-character buffer
  and numbers now go past five digits (python-sgp4 also refuses numbers
  above 339999; pass it a dummy).
- Prefer wrapping or configuring over editing anything in `third_party/`,
  because the vendored files are kept byte for byte as released (sha256 in
  `third_party/sgp4/README.md`) and the published test cases depend on them.
- Prefer one CelesTrak download per data update (about two hours), with no
  retries, over any retry loop, because CelesTrak blocks clients that repeat
  requests. Only `python/fetch_elements.py` downloads; never CI or tests.
- Prefer keeping a snapshot's SpaceX trajectory files over assuming they can
  be fetched again, because SpaceX replaces them several times a day and
  keeps no archive. They are not in git; `metadata.json` holds their sha256.
- Prefer finding shells and planes from positions propagated to one common time
  over the raw element values, because element sets have different epochs, plane
  orientations drift by degrees per day, and mean elements mean nothing outside
  SGP4.
- Prefer narrow height bands (a few km) for a shell over a wide one, because a
  satellite 1 km off slides about 1.2 degrees a day along the orbit against its
  shell, and several populations sit within 10 km of each other at 53 degrees.
- Prefer computing a laser link's longest length from the two heights over a
  fixed number, because the constellation was lowered in 2026 (about 4,500 km at
  today's heights, not the 5,000 km of the old 550 km shell).
- Prefer checking every gateway and PoP coordinate from the community dataset
  over trusting it, because it is US-only (45 gateways elsewhere are assumed at
  PoP sites) and its automatic place lookups put two PoPs in the wrong place.
- Prefer comparing with Hypatia through the independent Python model built with
  Hypatia's same-position grid over holding the program's +Grid to Hypatia's
  numbers, because on Hypatia's half-slot-staggered shell the nearest-distance
  +Grid is a different, shorter link set (about half the delay on Paris to
  Luanda). Hypatia's 15.19 revolutions per day is about 510 km, not 550.
- Prefer a non-sanitizer check executable for tests that read the full snapshot
  or the real ground files over a sanitizer test, because GCC's sanitizers make
  them take over a minute each. Keep new sanitizer tests on small shells for the
  same reason: one test on Hypatia's full shell took 11 minutes under GCC.
- Prefer taking longest-lived's candidate satellites per shell over one pair
  overall, because lasers never join two shells: candidates in different shells
  at the two ends gave New York to London a median of 270 ms over 71 hops.
- Prefer deciding whether a laser link is permanent from the widest angle the
  two satellites ever reach over the link's state at one instant, because a pair
  in sight at the reference time can be out of sight half an orbit later.
- Prefer building a plan from what the planner (or a satellite) knows over the true network,
  because after a failure the two differ until the news arrives, and that gap is what Phase 4
  measures. The truth decides only whether traffic gets through.
- Prefer one fixed list of laser links for a run (the planned ones, then every repair link the
  run will ever have, each with a status over time) over a list that grows while stepping,
  because link states, graphs and checks all index links by position.
- Prefer taking a satellite as down only from news that cannot have come from itself over "all
  its laser links are known down", because a working satellite whose neighbours all failed
  still carries traffic between two ground sites: that guess made every view wrong for good
  at a 30% failed share.
- Prefer exact IEEE arithmetic over `-ffast-math`, because it breaks the
  bit-level agreement with the published SGP4 cases and differs between
  arm64 (local) and x86-64 (CI).

## Working model: Fable orchestrates, Opus implements

A strategy being tried on this project. Revisit it if it isn't paying off.

- **Claude Fable 5.2 (the main session) is the orchestrator and architect.**
  It owns design, task breakdown, the briefs given to subagents, review of
  what they return, and verification.
- **Implementation goes to Opus 5.5 subagents** (`model: "opus"` on the Agent
  tool). Give each one a narrow, self-contained brief: the goal, the files it
  may touch, the constraints, and what done looks like (tests, check command).
- **Be selective with subagents, because they burn usage limits.** Spawn one
  only when there is a real, well-scoped implementation unit. Prefer one agent
  per coherent unit of work over wide fan-outs, and parallelize only pieces
  that are truly independent. Trivial edits, small fixes and anything that is
  a handful of tool calls are done directly by Fable rather than delegated.
- **Fable owns this codebase.** Delegating the typing does not delegate the
  responsibility: Fable is accountable for the code being correct, working and
  hygienic. Never accept a subagent's report at face value. Read the diff, run
  the check command, and fix or send back anything that is wrong, sloppy,
  untested or out of scope before it is committed.

## Working rules

The rules from the owner's global file that this project relies on.

- **Plan, implement, verify, commit.** For anything non-trivial write a short
  plan first, then verify by running `scripts/check.sh` for real. Never assume it
  passes. Cross-cutting plans are kept in `docs/`.
- **A bug becomes a failing test first.** Reproduce it as a test named after the
  bug, confirm it fails for the right reason, fix the code without editing that
  test, then run the full check.
- **Prove new tests can fail.** After a unit of work, plant bugs one at a time
  (never committed), run `scripts/check.sh asan-clang`, and confirm a named test
  fails on each.
- **Self-review before every commit:** only the intended files changed; every
  plan item is present; the check ran and is green; edge cases and data-critical
  arithmetic traced by hand on one example; line numbers, signatures and helpers
  re-read rather than remembered.
- **One commit per finished feature,** with a clear subject and the
  `Co-Authored-By` trailer of the model in use.
- **Decisions belong to the owner and are put to him in chat,** with the options
  and a recommendation. No open questions or option lists in specs or other
  committed files. `DECISIONS.md` holds only decisions he has made, in his words:
  edit it only when he asks, and commit it only when he says.
- **Stop and tell the owner** when a result contradicts one of his decisions or a
  tolerance in a spec. Never tune a result to match a published number.
- **Explain in simple words,** define each term at first use, and report what
  failed or was skipped as plainly as what passed.
- **Never run `/code-review ultra`** (it is billed separately); `high` is the
  default and the ceiling.
- **Confirm before destructive git operations:** history rewrites, hard resets,
  `git clean`, forced anything.

## Pushing, and working in a cloud session

- **On the owner's computer:** local commits only. Never push or open a pull
  request without his explicit OK, each time.
- **In a cloud session,** work that is not pushed is lost when the session ends.
  Work on a branch of its own and push that branch as often as needed. Never
  push to `main`, never merge into it and never open a pull request without the
  owner's explicit OK, each time.
- **Docker in the cloud:** it is installed but the daemon does not start by
  itself; run `dockerd` in the background (or have the environment's setup script
  do it) before `scripts/check.sh`. The image is rebuilt in every new session.
  After the session's worker restarts, `dockerd` can fail with "timeout waiting for
  containerd": stop any `containerd`, delete `/var/run/docker/containerd/containerd.pid` and
  `/var/run/docker.pid`, and start it again.
  If the image build fails at `pip install` with a certificate error (the cloud's proxy
  re-signs HTTPS), build it from a copy of the Dockerfile outside the repo that also copies
  `/root/.ccr/ca-bundle.crt` in and sets `PIP_CERT` to it, tag it `satsim-ci`, and run
  `scripts/ci.sh <step>` in it with the `docker run` line of `scripts/check.sh`. Never
  commit that copy.
- **Not in git, so not in the cloud:** a snapshot's SpaceX trajectory files
  (only `python/compare_spacex.py` needs them, and they cannot be downloaded
  again), the LENS ping files (`python/fetch_lens.py`; only
  `python/compare_lens.py` needs them), and everything under `results/` (the
  runners regenerate it). The committed summaries in `docs/phase-N-data/` hold
  the numbers the reports quote.

## Repo

- Public GitHub repo; GitHub is the source of truth.
- Never commit secrets: everything here is publicly visible.
