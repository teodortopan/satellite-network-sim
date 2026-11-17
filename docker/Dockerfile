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
        libeigen3-dev \
        ninja-build \
        nlohmann-json3-dev \
        python3 \
        python3-venv \
        valgrind \
    && rm -rf /var/lib/apt/lists/*

# Python tooling lives in a virtual environment: Ubuntu's system Python refuses pip installs.
COPY python/requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir -r /tmp/requirements.txt \
    && rm /tmp/requirements.txt
ENV PATH="/opt/venv/bin:${PATH}"

WORKDIR /work
