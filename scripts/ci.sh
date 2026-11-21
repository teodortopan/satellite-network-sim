#!/usr/bin/env bash
# CI steps. Runs inside the satsim-ci Docker image, from the repo root.
# Usage: scripts/ci.sh <format|python-lint|asan-gcc|asan-clang|valgrind|tidy|validate|validate-topology|validate-routing|all>
set -euo pipefail

usage() {
  echo "usage: $0 <format|python-lint|asan-gcc|asan-clang|valgrind|tidy|validate|validate-topology|validate-routing|all>" >&2
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
  mapfile -t files < <(our_sources | grep '\.cpp$')
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
    validate-routing)
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
