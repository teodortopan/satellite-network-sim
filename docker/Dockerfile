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

# Node for the dashboard (docs/phase-5-dashboard.md). Ubuntu's own package is too old for the
# page's build tools, so the official release is taken and checked against its sha256.
ARG NODE_VERSION=22.21.1
ARG NODE_SHA256_AMD64=680d3f30b24a7ff24b98db5e96f294c0070f8f9078df658da1bce1b9c9873c88
ARG NODE_SHA256_ARM64=e660365729b434af422bcd2e8e14228637ecf24a1de2cd7c916ad48f2a0521e1
RUN apt-get update \
    && apt-get install -y --no-install-recommends curl xz-utils \
    && rm -rf /var/lib/apt/lists/* \
    && case "$(dpkg --print-architecture)" in \
        amd64) node_arch=x64; node_sha256=$NODE_SHA256_AMD64 ;; \
        arm64) node_arch=arm64; node_sha256=$NODE_SHA256_ARM64 ;; \
        *) echo "no Node build pinned for this architecture" >&2; exit 1 ;; \
    esac \
    && curl -fsSL -o /tmp/node.tar.xz \
        "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-${node_arch}.tar.xz" \
    && echo "${node_sha256}  /tmp/node.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 --no-same-owner \
    && rm /tmp/node.tar.xz

# Chromium for the dashboard's browser test, with the system libraries it needs. The version
# has to equal @playwright/test in dashboard/package.json: each Playwright release runs one
# exact browser build (scripts/ci.sh checks the two agree).
ARG PLAYWRIGHT_VERSION=1.63.0
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers
ENV SATSIM_PLAYWRIGHT_VERSION=${PLAYWRIGHT_VERSION}
RUN npx --yes "playwright@${PLAYWRIGHT_VERSION}" install --with-deps chromium \
    && rm -rf /var/lib/apt/lists/* /root/.npm

WORKDIR /work
