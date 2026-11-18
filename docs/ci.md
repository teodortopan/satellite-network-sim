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
scripts/check.sh valgrind   # one step: format, python-lint, asan-gcc, asan-clang, valgrind, tidy
                            # or validate
```

Build trees go to `build/<step>` (for example `build/asan-clang`), including the GoogleTest
download.

## Jobs

| Job | What it catches | Why it exists |
|---|---|---|
| `format` | Any file in `include/`, `src/`, `apps/`, `tests/` that differs from `clang-format` output | Keeps diffs about behaviour, not whitespace |
| `python-lint` | `ruff check` findings (pyflakes, pycodestyle, import order, bugbear, pyupgrade, simplifications) and any file in `python/` that differs from `ruff format` output. Settings in `ruff.toml`, ruff version pinned in `python/requirements.txt` | The Python tooling (data download, validation, plots) gets the same lint and format bar as the C++ |
| `asan` (gcc, clang) | AddressSanitizer: heap/stack/global overflows, use-after-free, double free. LeakSanitizer: leaks at exit. UBSan: signed overflow, bad shifts, null or misaligned access, invalid casts, float-to-int overflow | The fast memory and UB check (~2x slowdown), so it runs every test. `-fno-sanitize-recover=all` makes the first report fail the test |
| `valgrind` | Memcheck: reads of uninitialised memory, which ASan does not detect, plus invalid accesses and leaks. `--trace-children=yes` also checks the `propagate` program that the end-to-end test starts; `--error-exitcode=99` (a status the program never uses) makes a Valgrind error in it fail that test | Runs on a separate unsanitized `RelWithDebInfo` (`-O2`) build, so tests also run on optimised code where some bugs only appear |
| `tidy` | clang-tidy `bugprone-*`, `performance-*`, most of `modernize-*` and `cppcoreguidelines-*`, and a set of `readability-*` checks on every `.cpp` in `src/`, `apps/` and `tests/`; any finding is an error | Catches suspicious code patterns the compiler accepts |
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

## Deliberately deferred

- **ThreadSanitizer**: nothing is multithreaded yet. Add a TSan job with the first thread.
- **Coverage reports**: not useful until there is enough code to measure.
- **Fuzzing**: add a libFuzzer target for the TLE parser once it exists.
