#!/usr/bin/env bash
# CI steps. Runs inside the satsim-ci Docker image, from the repo root.
# Usage: scripts/ci.sh <format|python-lint|asan-gcc|asan-clang|valgrind|tidy|validate|validate-topology|validate-routing|validate-failures|dashboard|dashboard-site|all>
set -euo pipefail

usage() {
  echo "usage: $0 <format|python-lint|asan-gcc|asan-clang|valgrind|tidy|validate|validate-topology|validate-routing|validate-failures|dashboard|dashboard-site|all>" >&2
}

# Our C++ sources (not third-party code under build/).
our_sources() {
  find include src apps tests -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort
}

configure_and_build() {
  local preset=$1 dir=$2 cc=$3 cxx=$4
  CC=$cc CXX=$cxx cmake --preset "$preset" -B "$dir"
  cmake --build "$dir"
}

step_format() {
  local files
  mapfile -t files < <(our_sources)
  clang-format --version
  clang-format --dry-run --Werror "${files[@]}"
  echo "format: ${#files[@]} files OK"
}

# Lint and formatting of the Python tooling. Settings are in ruff.toml.
step_python_lint() {
  ruff --version
  ruff check python
  ruff format --check python
}

run_asan() {
  local name=$1 cc=$2 cxx=$3
  local dir="build/asan-$name"
  configure_and_build asan "$dir" "$cc" "$cxx"
  ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    ctest --test-dir "$dir" --output-on-failure -j "$(nproc)"
}

step_valgrind() {
  local dir=build/valgrind
  configure_and_build valgrind "$dir" gcc g++
  ctest --test-dir "$dir" --output-on-failure -j "$(nproc)"

  local tests
  mapfile -t tests < <(find "$dir/tests" -maxdepth 1 -type f -perm -u+x -name '*_tests' | sort)
  if [[ ${#tests[@]} -eq 0 ]]; then
    echo "valgrind: no test executables found in $dir/tests" >&2
    return 1
  fi
  # --trace-children=yes: tests that run a built program (the propagate end-to-end test) get
  # it checked too. Exit code 99 is one no program here uses, so a test that checks a child's
  # exit status sees a Valgrind error in the child as a failure.
  # The executables run side by side, one per core, each into its own log; the logs are then
  # printed in order, and one failure fails the step.
  local logs
  logs=$(mktemp -d)
  local status=0
  printf '%s\n' "${tests[@]}" | xargs -P "$(nproc)" -I{} sh -c '
    valgrind --error-exitcode=99 --trace-children=yes --leak-check=full \
      --errors-for-leak-kinds=definite,possible --track-origins=yes "$1" \
      >"$2/$(basename "$1").log" 2>&1 || touch "$2/$(basename "$1").failed"
  ' sh {} "$logs" || status=1
  local t
  for t in "${tests[@]}"; do
    echo "valgrind: $t"
    cat "$logs/$(basename "$t").log"
    if [[ -e "$logs/$(basename "$t").failed" || ! -e "$logs/$(basename "$t").log" ]]; then
      echo "valgrind: $t FAILED" >&2
      status=1
    fi
  done
  rm -rf "$logs"
  return "$status"
}

step_tidy() {
  local dir=build/tidy
  CC=clang CXX=clang++ cmake --preset tidy -B "$dir"
  local files
  # apps/routing_wasm/ is compiled only by Emscripten (its main.cpp includes Emscripten's
  # headers), so it is not in this native compile_commands.json and is not tidied; clang-format
  # still checks it, and its build has our warnings as errors.
  mapfile -t files < <(our_sources | grep '\.cpp$' | grep -v '^apps/routing_wasm/')
  clang-tidy --version
  # Headers are checked through the .cpp files that include them (HeaderFilterRegex).
  # One clang-tidy per file, as many at once as there are cores; xargs exits non-zero if any
  # of them does.
  printf '%s\n' "${files[@]}" | xargs -P "$(nproc)" -n 1 clang-tidy -p "$dir" --quiet
  echo "tidy: ${#files[@]} files OK"
}

# The propagate program, optimised and without sanitizers, checked against Skyfield on a
# fixed sample of the committed snapshot (python/validate_against_reference.py). Tests are not
# built (no GoogleTest download), so the step needs no network once the image exists.
step_validate() {
  local dir=build/validate
  CC=gcc CXX=g++ cmake --preset valgrind -B "$dir" -DSATSIM_BUILD_TESTS=OFF
  cmake --build "$dir" --target propagate
  python3 python/validate_against_reference.py --propagate "$dir/bin/propagate"
}

# The topology and constellation programs, optimised and without sanitizers, on Hypatia's first
# Starlink shell (python/make_ideal_shell.py, written into a temporary directory): plane
# assignment, link counts and every best-case delay against an independent Python model, and
# Hypatia's published round-trip times (python/validate_topology.py). No network needed.
step_validate_topology() {
  local dir=build/validate
  CC=gcc CXX=g++ cmake --preset valgrind -B "$dir" -DSATSIM_BUILD_TESTS=OFF
  cmake --build "$dir" --target topology constellation
  python3 python/validate_topology.py --topology "$dir/bin/topology" \
    --constellation "$dir/bin/constellation"
}

# The routing program, optimised and without sanitizers, on Hypatia's and the Bariloche
# paper's shells with their link files (python/make_ideal_shell.py, written into a temporary
# directory): every row of three short runs (routes, drops, plans, delays) and every change
# count against an independent Python model (python/validate_routing.py). No network needed.
step_validate_routing() {
  local dir=build/validate
  CC=gcc CXX=g++ cmake --preset valgrind -B "$dir" -DSATSIM_BUILD_TESTS=OFF
  cmake --build "$dir" --target routing
  python3 python/validate_routing.py --routing "$dir/bin/routing"
}

# The failures program, optimised and without sanitizers, on Hypatia's shell with its +Grid
# and with a `three` plan made in Python (python/make_ideal_shell.py, written into a temporary
# directory): the model's own self-tests, then every interval, fact, flood, route, measure and
# network row of six short failure runs against an independent Python model
# (python/validate_failures.py). No network needed.
step_validate_failures() {
  local dir=build/validate
  CC=gcc CXX=g++ cmake --preset valgrind -B "$dir" -DSATSIM_BUILD_TESTS=OFF
  cmake --build "$dir" --target failures
  python3 python/validate_failures.py --self-test
  python3 python/validate_failures.py --failures-program "$dir/bin/failures"
}

# The programs the dashboard's export runs, optimised and without sanitizers.
build_export_programs() {
  local dir=$1
  CC=gcc CXX=g++ cmake --preset valgrind -B "$dir" -DSATSIM_BUILD_TESTS=OFF
  cmake --build "$dir" --target propagate constellation topology routing failures
}

# The routing program in WebAssembly (apps/routing_wasm), which the dashboard runs on demand:
# Emscripten from the image (docker/Dockerfile), Release (-O3, exact IEEE arithmetic, no
# -ffast-math), our warnings as errors, tests off. Eigen and nlohmann-json come from
# $SATSIM_WASM_DEPS, a directory without the host's C headers. Writes
# build/wasm/bin/satsim-routing.js and .wasm.
build_wasm_engine() {
  local dir=build/wasm
  "$SATSIM_EMSCRIPTEN/emcmake" cmake -S . -B "$dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSATSIM_BUILD_TESTS=OFF \
    -DSATSIM_WARNINGS_AS_ERRORS=ON \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
    -DEigen3_DIR="$SATSIM_WASM_DEPS/cmake/Eigen3" \
    -Dnlohmann_json_DIR="$SATSIM_WASM_DEPS/cmake/nlohmann_json"
  cmake --build "$dir" --target satsim-routing
}

# The page's packages, exactly as dashboard/package-lock.json lists them. The browser in the
# image belongs to one Playwright release, so a different one in the lock file is refused here
# and not as a missing-browser error in the middle of the test.
install_dashboard_packages() {
  local wanted
  wanted=$(node -p "require('./dashboard/package.json').devDependencies['@playwright/test']")
  if [[ $wanted != "$SATSIM_PLAYWRIGHT_VERSION" ]]; then
    echo "dashboard: @playwright/test is $wanted but the image has the browser of" \
      "$SATSIM_PLAYWRIGHT_VERSION (docker/Dockerfile)" >&2
    return 1
  fi
  (cd dashboard && npm ci --no-audit --no-fund)
}

# The dashboard (docs/phase-5-dashboard.md 6): the small export of real engine runs into
# dashboard/public/data (it replaces whatever export was there), with the routing program in
# WebAssembly and its inputs; the data check that compares every exported number with the
# engine's CSV files and measures the interpolation error; the engine check, which runs the
# exported WebAssembly program in Node and holds it to the route story and to the native
# routing program (dashboard/scripts/check-engine.ts); then the page's type check, lint, unit
# tests, build and browser test on that data. Only `npm ci` needs the network.
step_dashboard() {
  local dir=build/validate
  build_export_programs "$dir"
  build_wasm_engine
  python3 python/export_dashboard.py --small --bin "$dir/bin" --engine build/wasm/bin
  python3 python/check_dashboard_data.py --engine build/wasm/bin
  install_dashboard_packages
  node dashboard/scripts/check-engine.ts --routing "$dir/bin/routing"
  cd dashboard
  npm run typecheck
  npm run lint
  npm test
  npm run build
  npm run e2e -- --trace off
}

# The site as it is published: the full export, its data check, and the built page in
# dashboard/dist. Not part of `all`; the Pages workflow runs it.
step_dashboard_site() {
  local dir=build/validate
  build_export_programs "$dir"
  build_wasm_engine
  python3 python/export_dashboard.py --bin "$dir/bin" --engine build/wasm/bin
  python3 python/check_dashboard_data.py --engine build/wasm/bin
  install_dashboard_packages
  (cd dashboard && npm run build)
}

run_step() {
  echo "==> $1"
  case "$1" in
    format) step_format ;;
    python-lint) step_python_lint ;;
    asan-gcc) run_asan gcc gcc g++ ;;
    asan-clang) run_asan clang clang clang++ ;;
    valgrind) step_valgrind ;;
    tidy) step_tidy ;;
    validate) step_validate ;;
    validate-topology) step_validate_topology ;;
    validate-routing) step_validate_routing ;;
    validate-failures) step_validate_failures ;;
    dashboard) step_dashboard ;;
    dashboard-site) step_dashboard_site ;;
    *)
      usage
      exit 2
      ;;
  esac
}

main() {
  if [[ $# -ne 1 ]]; then
    usage
    exit 2
  fi

  if [[ $1 != all ]]; then
    run_step "$1"
    return
  fi

  local steps=(format python-lint asan-gcc asan-clang valgrind tidy validate validate-topology
    validate-routing validate-failures dashboard)
  local failed=() s
  for s in "${steps[@]}"; do
    # Separate process so one failing step does not stop the others. (A subshell in an
    # `if` condition would silently disable set -e inside the step.)
    if bash "$0" "$s"; then
      echo "==> $s: PASS"
    else
      echo "==> $s: FAIL"
      failed+=("$s")
    fi
  done

  echo
  echo "Summary: $((${#steps[@]} - ${#failed[@]}))/${#steps[@]} steps passed"
  if [[ ${#failed[@]} -gt 0 ]]; then
    echo "Failed: ${failed[*]}"
    exit 1
  fi
}

main "$@"
