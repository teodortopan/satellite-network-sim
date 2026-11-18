# satellite-network-sim

A simulator of a Starlink-like satellite network on real public orbital data.

Global rules (workflow, commits, review, self-review) load from
`~/.claude/CLAUDE.md`. Project-specific rules are below. The Phase 1 spec
(`docs/phase-1-orbits.md`) has a glossary of every orbital term used here.

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
  clang-tidy, and `validate` (the `propagate` program against Skyfield on the
  committed snapshot). Exit 0 only if all pass. It needs only Docker on the host.
- `scripts/check.sh <step>` runs one step (`format`, `python-lint`,
  `asan-gcc`, `asan-clang`, `valgrind`, `tidy`, `validate`).
- `python/compare_spacex.py` (against SpaceX's trajectory files) and
  `python/plot_phase1.py` (the report's plots) are run by hand; the exact
  commands are in `docs/phase-1-results.md`.
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
| `apps/propagate/` | The `propagate` program: snapshot and time range in, CSV out |
| `tests/<module>/` | GoogleTest per module, with small fixtures |
| `python/` | Snapshot download, reference generation, validation, SpaceX comparison, plots |
| `data/starlink/<date>/` | One dated snapshot: element sets, IERS file, trajectory checksums |

Later modules may depend on earlier ones (core, time, frames, orbit), never
the reverse.

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

## Repo

- Public GitHub repo; GitHub is the source of truth.
- Never commit secrets: everything here is publicly visible.
