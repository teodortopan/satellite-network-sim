# Canonical build environment for satsim. Used by scripts/check.sh locally and
# by GitHub Actions. No sources are copied in: the repo is bind-mounted at /work.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        clang-format \
        clang-tidy \
        cmake \
        git \
        libclang-rt-dev \
        ninja-build \
        valgrind \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
