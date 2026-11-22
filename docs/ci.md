# CI and local checks

All checks run inside one Docker image (`docker/Dockerfile`, Ubuntu 24.04 with GCC 13,
Clang 18, CMake, Ninja, Valgrind, clang-format, clang-tidy, and Python 3 with the packages
pinned in `python/requirements.txt`: ruff, pyerfa, Skyfield, python-sgp4, matplotlib). GitHub
Actions and the local check run the same `scripts/ci.sh` steps in that image, so a green
local run means the same commands pass in CI. Local runs on an Apple silicon Mac are linux/arm64; CI is x86_64.

## Running locally

Only Docker is needed on the host.

```sh
scripts/check.sh            # every step; exit 0 only if all pass
scripts/check.sh valgrind   # one step: format, python-lint, asan-gcc, asan-clang, valgrind, tidy,
                            # validate, validate-topology, validate-routing or
                            # validate-failures
```

Build trees go to `build/<step>` (for example `build/asan-clang`), including the GoogleTest
download.

The tests of the sanitizer and Valgrind steps, the test executables under Valgrind and the
files under clang-tidy each run side by side, one per core (`nproc`). Nothing is skipped: one
failing test, executable or file still fails its step. On 4 cores this took the Valgrind step
from 215 s to 60 s and clang-tidy from 19 minutes to 8.

## Jobs

| Job | What it catches | Why it exists |
|---|---|---|
| `format` | Any file in `include/`, `src/`, `apps/`, `tests/` that differs from `clang-format` output | Keeps diffs about behaviour, not whitespace |
| `python-lint` | `ruff check` findings (pyflakes, pycodestyle, import order, bugbear, pyupgrade, simplifications) and any file in `python/` that differs from `ruff format` output. Settings in `ruff.toml`, ruff version pinned in `python/requirements.txt` | The Python tooling (data download, validation, plots) gets the same lint and format bar as the C++ |
| `asan` (gcc, clang) | AddressSanitizer: heap/stack/global overflows, use-after-free, double free. LeakSanitizer: leaks at exit. UBSan: signed overflow, bad shifts, null or misaligned access, invalid casts, float-to-int overflow | The fast memory and UB check (~2x slowdown), so it runs every test. `-fno-sanitize-recover=all` makes the first report fail the test |
| `valgrind` | Memcheck: reads of uninitialised memory, which ASan does not detect, plus invalid accesses and leaks. `--trace-children=yes` also checks the `propagate` program that the end-to-end test starts; `--error-exitcode=99` (a status the program never uses) makes a Valgrind error in it fail that test | Runs on a separate unsanitized `RelWithDebInfo` (`-O2`) build, so tests also run on optimised code where some bugs only appear |
| `tidy` | clang-tidy `bugprone-*`, `performance-*`, most of `modernize-*` and `cppcoreguidelines-*`, and a set of `readability-*` checks on every `.cpp` in `src/`, `apps/` and `tests/`; any finding is an error | Catches suspicious code patterns the compiler accepts |
| `validate-topology` | `topology` and `constellation` output on an ideal shell that disagrees with an independent Python model: a satellite in the wrong plane or slot, an element set from the wrong file, a wrong count of satellites, planes or laser links, a laser left without a partner, a different laser grid, elevation mask, ground link or shortest path (any row's status, laser hops, first or last satellite, or a one-way delay more than 1e-6 ms off). Builds both programs optimised without sanitizers and runs `python/validate_topology.py` | The unit tests check each piece of the topology on small hand-made cases; this checks the whole program on a full 1,584-satellite shell against code written separately (positions from python-sgp4 and Skyfield, the +Grid built from plane and slot indices, its own Dijkstra), and checks that model against the round-trip times the Hypatia paper published. Tolerances and measured values: `docs/phase-2-results.md` |
| `validate-routing` | `routing` output on two ideal published shells that disagrees with an independent Python model: any row's status, drop cause, route (every satellite in order), plan (kept, replaced, none), laser hops, first or last satellite, or a one-way or ideal delay more than 1e-9 ms off; or a summary count of bad changes, delay increases, breaks or recoveries. Covers `lowest-delay`, `keep-until-broken` and `longest-lived`, routes held between plans, link files, `--max-ground-range-km`, `--min-elevation-deg`, `--hop-delay-ms`, sub-second steps and `--directions forward`. Builds `routing` optimised without sanitizers and runs `python/validate_routing.py` | The unit tests check each strategy on small hand-worked graphs; this checks the whole program on full 1,584-satellite shells against a re-plan loop, graph and Dijkstra written separately from the spec (positions from python-sgp4 with its own TEME to ITRF, checked against Skyfield). It is the comparison the published reproductions rest on (`docs/phase-3-routing.md` 6.3) |
| `validate-failures` | `failures` output on Hypatia's ideal shell that disagrees with an independent Python model: any interval of the events CSV (state, route, drop cause, failed element, start and end to 1e-9 s, delay to 1e-9 ms), any fact (subject, down or up, instant, old news, detectors and their instants, when the planner knew, reports and their hops, flood nodes, last learning instant, messages), any node's learning instant in a flood, any routes CSV row (state, cause, element, route, delay, ideal delay, plan), any measure of a lane to 1e-9, or any network row (working satellites, pieces, largest piece). Covers satellite and laser-link failures, permanent, transient and old news, the three reaction modes and the baseline and reference lanes, `lowest-delay` and `keep-until-broken`, the `start` and `whole-interval` planners, `--replan-on-report`, re-pointing with in-plane and cross-plane repair links, and both a +Grid and a `three` link file. Runs the model's own self-tests first. Builds `failures` optimised without sanitizers and runs `python/validate_failures.py` | The unit tests check each piece of the failure engine on hand-made geometry; this checks the whole program on a full 1,584-satellite shell against detection, reports, floods, views, the five lanes and re-matching written separately from the spec. It is the proof of Phase 4 (`docs/phase-4-failures.md` 6.3) |
| `validate` | `propagate` output that disagrees with Skyfield on real data: wrong time since epoch, GMST, UT1, polar motion, geodetic conversion or J2000 chain, a wrong element set chosen, a row missing or extra. Builds `propagate` optimised without sanitizers (tests off, so nothing is downloaded) and runs `python/validate_against_reference.py` on a fixed sample of the committed snapshot | The unit tests check each piece against printed references; this checks the whole program against an independent implementation, row by row, on the data it is meant for. Tolerances and measured values: `docs/phase-1-results.md` |

Every build also compiles our code with
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`. `-Wconversion`
matters for numeric code: a silent `double` to `float` or signed to unsigned narrowing gives
plausible but wrong results. Building with both GCC and Clang catches code that only compiles
on one of them, and each compiler warns about different things. GoogleTest is built with the
same sanitizer flags as our code but without our warnings.

The disabled clang-tidy checks and the reason for each are listed at the top of `.clang-tidy`.

## The full-snapshot check

Unit tests run under the sanitizers and Valgrind on small fixtures of real records. Loading
the whole committed Starlink snapshot (about 22,000 records) is a check on the data, not on
memory safety, so it is a separate executable, `satsim_orbit_snapshot_check`. It is built
only without sanitizers and runs natively in the `valgrind` step's optimised build, where it
takes a fraction of a second. Under GCC's AddressSanitizer the same parse takes about five
minutes per file, because GCC enables stack-use-after-return detection by default and the
JSON parser makes very many small calls. If a real record ever fails to load, copy it into
the fixtures so the sanitizers cover that path too.

## The validate step

`python/validate_against_reference.py` runs `propagate` on 277 satellites at 40 times over
about three days (11,061 rows) and recomputes every row with Skyfield from the same element
sets and the same `finals2000A.all`. It takes about 5 seconds after the build. It reads only
local files: the script makes any network connection raise, and the GitHub job runs the
container with `--network none`. The SpaceX comparison (`python/compare_spacex.py`) is not
a CI step: it needs trajectory files that are not in git.

## The validate-topology step

`python/make_ideal_shell.py` writes Hypatia's first Starlink shell (72 planes of 22
satellites, 53 degrees, Hypatia's own element values and phasing, epoch 2000-01-01) into a
temporary directory: the three snapshot files and a shell definition read with
`--shells-file`. `python/validate_topology.py` then runs `constellation` and `topology`
(`plus-grid`, `best-case`, every pair of `data/ground/cities.json`, one hour every minute and
200 s every second) and recomputes everything independently. It takes about 10 seconds after
the build, reads only local files (any network connection raises) and runs with
`--network none` in the GitHub job. Which file each satellite's element set comes from is
arranged so that a program that picked the wrong one would fail (the script's docstring says
how).

The step fails if the program and the model differ, and also if the model, built with
Hypatia's own laser grid, is more than 20% from Hypatia's published Paris to Luanda round-trip
times. The program's own values on that shell are printed but not held to Hypatia's: its
nearest-distance laser matching gives a different grid from Hypatia's on this shell
(`docs/phase-2-results.md` explains why and by how much).

## The validate-routing step

`python/make_ideal_shell.py` writes two shells with their link files into a temporary
directory: Hypatia's (as above) and the Bariloche paper's (Walker phase offset 39, 550 km,
the index shifted by 39 at the wrap from the last plane to the first).
`python/validate_routing.py` runs `routing` (best case, `start` planner) on three cases and
recomputes every row:

| Case | Shell, ground rule | Cities | Time | Strategies |
|---|---|---|---|---|
| `hypatia` | Hypatia, within 1,089.686 km | `data/ground/cities.json`, both ways | 300 s every 1 s, re-planned every step | the three |
| `bariloche` | Bariloche, 40 deg mask, 1.012 ms per hop | the same plus Bariloche to Beijing | 600 s every 1 s, re-planned every 5 s | the three |
| `hypatia-0.1s` | Hypatia, within 1,089.686 km | Hypatia's first 10 cities (`data/ground/hypatia/`), one way | 10 s every 0.1 s | `lowest-delay` |

The model takes positions from python-sgp4 on the elements by construction, with its own
TEME to ITRF (checked against Skyfield to 1 mm; measured 0.003 mm), and shares no code with the
program. The routes must be identical and the delays within 1e-9 ms (measured: 1.1e-12 ms at
most). It takes about 20 seconds after the build, reads only local files (any network
connection raises) and runs with `--network none` in the GitHub job.

## The validate-failures step

`python/make_ideal_shell.py` writes Hypatia's shell into a temporary directory with two
four-column link files (the laser side at each end): its +Grid, and a `three` plan made by
the Python planner written from Phase 2 spec 4.3 (2,376 links, no laser without a partner).
`python/validate_failures.py` first runs its self-tests (hand-worked cases on tiny made-up
networks, and the no-failure baseline against `validate_routing.py`'s model), then runs
`failures` (best case, Hypatia's ground range, New York to London and Frankfurt to Singapore
one way, `lowest-delay` and `keep-until-broken`, all three reaction modes) on six cases and
recomputes every output. The failures are picked from the baseline route at 7 s, after a
re-plan at 0 s and before the next at 15 s:

| Case | Links | Failure | Settings |
|---|---|---|---|
| `satellite-three` | `three` | the route's middle satellite, permanent | 40 s, `whole-interval` |
| `laser-grid` | +Grid | the route's middle laser link, permanent | 40 s, `whole-interval` |
| `transient-three` | `three` | the middle laser link for 10 s | acquisition 5 s |
| `repoint-three` | `three` | two satellites whose cross-plane partners face each other | `--repoint on`, acquisition 2 s |
| `report-three` | `three` | the route's middle satellite | `--replan-on-report on`, `start`, 25 s |
| `after-recovery` | `three` | 3% of the satellites, an hour before the start | one step, `--repoint on` |

The model shares no code with the program; positions come from the same python-sgp4 code as
`validate-routing`. Instants must agree to 1e-9 s and delays to 1e-9 ms. It takes about a
minute after the build (measured 53 s on 4 cores), reads only local files (any network
connection raises) and runs with `--network none` in the GitHub job.

## Deliberately deferred

- **ThreadSanitizer**: nothing is multithreaded yet. Add a TSan job with the first thread.
- **Coverage reports**: not useful until there is enough code to measure.
- **Fuzzing**: add a libFuzzer target for the TLE parser once it exists.
