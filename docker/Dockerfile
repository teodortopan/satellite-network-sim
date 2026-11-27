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

# Emscripten for the routing program in WebAssembly, which the dashboard runs on demand
# (apps/routing_wasm). The official release build of one tagged version (6.0.13), checked
# against its sha256, in /opt/emsdk; it is not on PATH, so its own clang never replaces
# Ubuntu's (scripts/ci.sh calls $SATSIM_EMSCRIPTEN/emcmake). Its system libraries are the
# release's prebuilt cache; building a small program with the engine's flags fills in any it
# lacks, so CI does not compile libc++ on every run. Tools the build never uses (a language
# server, a dependency scanner, fuzzers, the test suite) are removed to keep the image small.
ARG EMSCRIPTEN_VERSION=6.0.13
ARG EMSCRIPTEN_RELEASE=eb132757325583b787fe9414b09afea9442b72ad
ARG EMSCRIPTEN_SHA256_AMD64=cda6801012ff3319a539111826ed02a9abd7eff135c2ef261c8941caf5a3763e
ARG EMSCRIPTEN_SHA256_ARM64=18cd1b9df0ef1b6c35648d24cffc8596374a9c02f943ce8e1d2cf468de5033b5
ENV EM_CONFIG=/opt/emsdk/emscripten.config
ENV EM_CACHE=/opt/emsdk/emscripten/cache
ENV SATSIM_EMSCRIPTEN=/opt/emsdk/emscripten
RUN case "$(dpkg --print-architecture)" in \
        amd64) em_file=wasm-binaries.tar.xz; em_sha256=$EMSCRIPTEN_SHA256_AMD64 ;; \
        arm64) em_file=wasm-binaries-arm64.tar.xz; em_sha256=$EMSCRIPTEN_SHA256_ARM64 ;; \
        *) echo "no Emscripten build pinned for this architecture" >&2; exit 1 ;; \
    esac \
    && curl -fsSL -o /tmp/emsdk.tar.xz \
        "https://storage.googleapis.com/webassembly/emscripten-releases-builds/linux/${EMSCRIPTEN_RELEASE}/${em_file}" \
    && echo "${em_sha256}  /tmp/emsdk.tar.xz" | sha256sum -c - \
    && mkdir -p /opt/emsdk \
    && tar -xJf /tmp/emsdk.tar.xz -C /opt/emsdk --strip-components=1 --no-same-owner \
    && rm /tmp/emsdk.tar.xz \
    && grep -qx "${EMSCRIPTEN_VERSION}-git" /opt/emsdk/emscripten/emscripten-version.txt \
    && rm -rf /opt/emsdk/bin/clangd /opt/emsdk/bin/clang-scan-deps /opt/emsdk/bin/wasm-fuzz-* \
        /opt/emsdk/bin/wasm-reduce /opt/emsdk/emscripten/test \
    && printf '%s\n' "LLVM_ROOT = '/opt/emsdk/bin'" "BINARYEN_ROOT = '/opt/emsdk'" \
        "EMSCRIPTEN_ROOT = '/opt/emsdk/emscripten'" "NODE_JS = '/usr/local/bin/node'" \
        > "$EM_CONFIG" \
    && printf '%s\n' '#include <stdexcept>' '#include <string>' \
        'int main(int argc, char**) { try { if (argc > 5) throw std::runtime_error("x"); }' \
        'catch (const std::exception& e) { return static_cast<int>(std::string(e.what()).size()); }' \
        'return 0; }' > /tmp/warm.cpp \
    && "$SATSIM_EMSCRIPTEN/em++" -O3 -fwasm-exceptions -sMODULARIZE=1 -sEXPORT_ES6=1 \
        -sENVIRONMENT=web,worker,node -sINVOKE_RUN=0 -sEXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8 \
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB \
        /tmp/warm.cpp -o /tmp/warm.js \
    && rm -f /tmp/warm.cpp /tmp/warm.js /tmp/warm.wasm

# The header-only Eigen and nlohmann-json, for the WebAssembly build only, in a directory that
# holds nothing else: their Ubuntu package configurations point at /usr/include, and the host's
# C headers there must never reach a WebAssembly compile. Same headers, same version files.
ENV SATSIM_WASM_DEPS=/opt/wasm-deps
RUN mkdir -p /opt/wasm-deps/include /opt/wasm-deps/cmake/Eigen3 /opt/wasm-deps/cmake/nlohmann_json \
    && cp -r /usr/include/eigen3/Eigen /usr/include/eigen3/unsupported /usr/include/nlohmann \
        /opt/wasm-deps/include/ \
    && cp /usr/share/eigen3/cmake/Eigen3ConfigVersion.cmake /opt/wasm-deps/cmake/Eigen3/ \
    && cp /usr/share/cmake/nlohmann_json/nlohmann_jsonConfigVersion.cmake \
        /opt/wasm-deps/cmake/nlohmann_json/ \
    && printf '%s\n' 'if(NOT TARGET Eigen3::Eigen)' \
        '  add_library(Eigen3::Eigen INTERFACE IMPORTED)' \
        '  target_include_directories(Eigen3::Eigen SYSTEM INTERFACE /opt/wasm-deps/include)' \
        'endif()' > /opt/wasm-deps/cmake/Eigen3/Eigen3Config.cmake \
    && printf '%s\n' 'if(NOT TARGET nlohmann_json::nlohmann_json)' \
        '  add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)' \
        '  target_include_directories(nlohmann_json::nlohmann_json SYSTEM INTERFACE' \
        '    /opt/wasm-deps/include)' \
        'endif()' > /opt/wasm-deps/cmake/nlohmann_json/nlohmann_jsonConfig.cmake

WORKDIR /work
