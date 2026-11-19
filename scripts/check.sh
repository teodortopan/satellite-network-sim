#!/usr/bin/env bash
# Single check command. Builds the CI image and runs scripts/ci.sh in it.
# Requires only Docker on the host.
# Usage: scripts/check.sh [format|python-lint|asan-gcc|asan-clang|valgrind|tidy|validate|
#                          validate-topology|all]
# Default: all.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

docker build -t satsim-ci -f "$repo/docker/Dockerfile" "$repo"
docker run --rm --cap-add=SYS_PTRACE -v "$repo":/work -w /work satsim-ci \
  scripts/ci.sh "${1:-all}"
