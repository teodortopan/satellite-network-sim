# CI and local checks

All checks run inside one Docker image (`docker/Dockerfile`, Ubuntu 24.04 with GCC 13,
Clang 18, CMake, Ninja, Valgrind, clang-format, clang-tidy, and Python 3 with ruff). GitHub
Actions and the local check run the same `scripts/ci.sh` steps in that image, so a green
local run means the same commands pass in CI. Local runs on an Apple silicon Mac are linux/arm64; CI is x86_64.

## Running locally

Only Docker is needed on the host.

```sh
scripts/check.sh            # every step; exit 0 only if all pass
scripts/check.sh valgrind   # one step: format, python-lint, asan-gcc, asan-clang, valgrind or tidy
```

Build trees go to `build/<step>` (for example `build/asan-clang`), including the GoogleTest
download.

## Jobs

| Job | What it catches | Why it exists |
|---|---|---|
| `format` | Any file in `include/`, `src/`, `tests/` that differs from `clang-format` output | Keeps diffs about behaviour, not whitespace |
| `python-lint` | `ruff check` findings (pyflakes, pycodestyle, import order, bugbear, pyupgrade, simplifications) and any file in `python/` that differs from `ruff format` output. Settings in `ruff.toml`, ruff version pinned in `python/requirements.txt` | The Python tooling (data download, validation, plots) gets the same lint and format bar as the C++ |
| `asan` (gcc, clang) | AddressSanitizer: heap/stack/global overflows, use-after-free, double free. LeakSanitizer: leaks at exit. UBSan: signed overflow, bad shifts, null or misaligned access, invalid casts, float-to-int overflow | The fast memory and UB check (~2x slowdown), so it runs every test. `-fno-sanitize-recover=all` makes the first report fail the test |
| `valgrind` | Memcheck: reads of uninitialised memory, which ASan does not detect, plus invalid accesses and leaks | Runs on a separate unsanitized `RelWithDebInfo` (`-O2`) build, so tests also run on optimised code where some bugs only appear |
| `tidy` | clang-tidy `bugprone-*`, `performance-*`, most of `modernize-*` and `cppcoreguidelines-*`, and a set of `readability-*` checks; any finding is an error | Catches suspicious code patterns the compiler accepts |

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

## Deliberately deferred

- **ThreadSanitizer**: nothing is multithreaded yet. Add a TSan job with the first thread.
- **Coverage reports**: not useful until there is enough code to measure.
- **Fuzzing**: add a libFuzzer target for the TLE parser once it exists.
