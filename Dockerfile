# syntax=docker/dockerfile:1
#
# Multi-stage: one image serves both the aggregator and the clients. Compose
# supplies the command per service, so there is deliberately no CMD here.

# ---------- builder ----------
FROM ubuntu:24.04 AS builder

# ninja + pkg-config for the build; git/curl/zip/tar for vcpkg's fetching;
# ca-certificates so vcpkg can clone over HTTPS. ccache persists compiled
# object files across builds (see the build step below).
#
# build-essential stays for make/autotools, which some vcpkg ports still shell
# out to, but it is NOT the compiler used - see the CC/CXX block below.
#
# libclang-rt-18-dev is REQUIRED and is NOT a dependency of clang-18: the
# package holds compiler-rt, which is where the ASan/UBSan/TSan runtimes live.
# Without it clang compiles -fsanitize=... happily and then fails at LINK time
# with a missing __asan_* symbol, which reads like a project bug rather than a
# missing package.
#
# clang-tools-18 is REQUIRED for the same "missing package, not a code bug"
# reason: it provides clang-scan-deps-18. CMake + Ninja + C++20 + Clang turns
# on C++20 module dependency scanning, which runs clang-scan-deps before every
# translation unit. Without the package CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS is
# ...-NOTFOUND and ninja tries to exec that literal string, so EVERY compile
# fails - including the four tiny FindThreads probe programs, which surfaces as
# a misleading "Could NOT find Threads (missing: Threads_FOUND)". GCC 13 never
# enabled this scan path, so it only appeared after the switch to Clang.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang clang-tools-18 libclang-rt-18-dev \
        cmake ninja-build git curl zip unzip tar pkg-config \
        ca-certificates linux-libc-dev ccache \
    && rm -rf /var/lib/apt/lists/*

# Clang, not the GCC that build-essential provides, and set BEFORE the vcpkg
# install below so the dependencies are built with the same compiler as the
# project.
#
# The reason is tsan.supp. That file is checked in and was authored against
# CLANG's ThreadSanitizer - it suppresses one known false positive in gRPC's
# ThreadManager teardown, verified with print_suppressions=1. GCC's TSan is a
# fork of the same runtime that symbolizes differently, so under GCC the
# suppression can either fail to match (the run drowns in a false positive
# already investigated) or over-match (it silently hides a real race). Neither
# failure announces itself, and the Dockerized run is the authoritative one -
# it is what a reviewer executes, and the only place LeakSanitizer runs at all.
#
# The cost, paid once: changing the compiler invalidates the vcpkg binary cache
# (a different compiler is a different ABI hash), so the first build after this
# recompiles gRPC, Boost, OpenSSL and Protobuf from source.
ENV CC=clang
ENV CXX=clang++

ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_MAX_CONCURRENCY=8
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} \
    && ${VCPKG_ROOT}/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /src

# Dependencies FIRST, from the manifest alone. gRPC, Boost, OpenSSL and
# Protobuf all compile from source, so this layer is the expensive one
# (~30-60 min cold). Keeping it above the source copy means editing a .cpp
# reuses it instead of rebuilding gRPC.
COPY vcpkg.json ./

# The cache mount is what makes a vcpkg.json change survivable. vcpkg stores
# built packages in ~/.cache/vcpkg/archives; without the mount that lives
# inside the layer, so invalidating this layer rebuilds gRPC, Boost and
# OpenSSL from source - another 30-60 min for a one-line manifest change.
# With it, only the newly added package builds.
#
# --clean-after-build discards build trees (large) but keeps the binary cache
# (the part worth persisting).
RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    ${VCPKG_ROOT}/vcpkg install --clean-after-build

# Source second - changes here reuse the dependency layer above.
COPY CMakeLists.txt ./
COPY types/ types/
COPY config/ config/
COPY logger/ logger/
COPY md_core/ md_core/
COPY md_provider/ md_provider/
# REQUIRED: the top-level CMakeLists add_subdirectory()s md_wire, so a missing
# copy fails at CONFIGURE time - which happens below, AFTER the 30-60 minute
# vcpkg layer. Listed in the same order as the add_subdirectory calls so the
# next addition is easy to spot as missing.
COPY md_wire/ md_wire/
COPY md_proto/ md_proto/
COPY aggregator/ aggregator/
COPY client/ client/
COPY tests/ tests/
# REQUIRED by the tsan stage, harmless everywhere else: tests/unit_tests/
# CMakeLists.txt points TSAN_OPTIONS at ${CMAKE_SOURCE_DIR}/tsan.supp when
# SANITIZER=thread. Without this copy the tsan build configures fine and then
# drowns every run in false positives from uninstrumented gRPC internals.
COPY tsan.supp ./

# VCPKG_MANIFEST_INSTALL=OFF: the dependency layer above already ran
# `vcpkg install` into /src/vcpkg_installed. Without this flag the vcpkg
# toolchain runs `install` AGAIN at CMake configure time. That rerun is not
# guaranteed to hit the binary cache, so it can rebuild gRPC/Boost from source
# - 15+ min - on every single source edit. OFF makes CMake trust the tree that
# is already there; VCPKG_INSTALLED_DIR points it at that tree.
#
# The two cache mounts stay as a safety net (a stray manifest change still
# lands in the persistent binary cache instead of the layer). CCACHE_DIR is a
# cache mount too: after this, fixing one .cpp recompiles one file, not all 52.
ENV CCACHE_DIR=/root/.cache/ccache
RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    --mount=type=cache,target=/root/.cache/ccache \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_INSTALL=OFF \
        -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    && cmake --build build -j

# ---------- test stages ----------
#
# All three keep the BUILDER filesystem rather than the slim runtime: they need
# cmake, ctest and the build tree, none of which belong in a shipping image.
# They are reached only via `target:` from the `tests` compose profile, so a
# plain `docker compose build` never materializes them.
#
# ORDERING MATTERS: these sit ABOVE the runtime stage on purpose. A Dockerfile
# with no --target builds the LAST stage, so appending them after runtime would
# make `docker compose up` silently build a sanitizer image. docker-compose.yml
# also pins `target: runtime` on the shared anchor, belt and braces.

# ---------- tests: release ----------
# Free. BUILD_TESTS defaults ON and tests/ is copied above, so `cmake --build
# build` in the builder ALREADY produced build/tests/unit_tests/unit_tests.
# This stage adds no compilation at all - only a different tag and a command.
FROM builder AS tests
WORKDIR /src

# ---------- tests: address + undefined sanitizer ----------
# A separate build tree, not a flag on the existing one: ASan and TSan are
# mutually exclusive (see the SANITIZER block in CMakeLists.txt), and neither
# can share a tree with Release. The expensive vcpkg layer above IS reused, so
# this recompiles our ~52 translation units and nothing else - gRPC, Boost and
# OpenSSL stay as built.
#
# Those vcpkg libraries are NOT instrumented. That is fine for ASan: our own
# code is the subject, and an overflow in our buffers is still caught.
#
# CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST moves gtest's test
# enumeration from build time to `ctest` time. By default gtest_discover_tests
# RUNS the test binary as a POST_BUILD step - i.e. inside `docker build`, under
# the daemon's default seccomp profile, which blocks what a sanitizer runtime
# needs to map its shadow memory. Deferring discovery means the binary first
# executes inside the container, where compose grants seccomp:unconfined.
FROM builder AS builder-asan
WORKDIR /src
RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    --mount=type=cache,target=/root/.cache/ccache \
    cmake -S . -B build_asan -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DSANITIZER=address \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_INSTALL=OFF \
        -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
        -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST \
    && cmake --build build_asan -j --target unit_tests

# ---------- tests: thread sanitizer ----------
# Same shape as the ASan stage. The suppressions file is wired in by
# tests/unit_tests/CMakeLists.txt as a test PROPERTY, which is why these
# services run `ctest` rather than the gtest binary directly - invoking the
# binary by hand loses TSAN_OPTIONS and reports gRPC's teardown as a race.
FROM builder AS builder-tsan
WORKDIR /src

# llvm-symbolizer is REQUIRED here and is NOT pulled in by clang/clang-tools-18.
# Clang's ThreadSanitizer needs it to turn stack frames into function names;
# without it every frame is "<null>" and the symbol-name entries in tsan.supp
# (race:grpc::*, race:absl::*, race:zmq::* ...) silently stop matching, so the
# run drowns in the uninstrumented-library false positives they exist to hide.
# Installed in its own layer AFTER `FROM builder` so it does not touch the
# shared apt line above and therefore does not invalidate the vcpkg cache.
# Sanitizers auto-discover `llvm-symbolizer` on PATH, so no TSAN_OPTIONS change.
RUN apt-get update && apt-get install -y --no-install-recommends llvm-18 \
    && ln -sf /usr/lib/llvm-18/bin/llvm-symbolizer /usr/bin/llvm-symbolizer \
    && rm -rf /var/lib/apt/lists/*

RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    --mount=type=cache,target=/root/.cache/ccache \
    cmake -S . -B build_tsan -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DSANITIZER=thread \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_INSTALL=OFF \
        -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
        -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST \
    && cmake --build build_tsan -j --target unit_tests

# ---------- runtime ----------
FROM ubuntu:24.04 AS runtime

# ca-certificates is REQUIRED, not optional: the aggregator opens TLS
# connections to the exchanges, and without a CA bundle every handshake fails
# in a way that looks like a network problem rather than a missing package.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
# Two aggregator binaries in one image, command chosen per compose service
# :
#   md_core_app     - Core + gRPC + ZmqCoreIngress (the ROUTER), no providers
#   md_provider_app - one provider per (venue, market), dials the ingress
COPY --from=builder /src/build/aggregator/md_core_app ./
COPY --from=builder /src/build/aggregator/md_provider_app ./
COPY --from=builder /src/build/client/client_app ./

# The ipc:// endpoint md_core binds and each md_provider dials
# (ipc:///run/md/core.ipc by default). compose mounts a shared volume here so
# the containers see one socket file; this mkdir is for a plain `docker run`,
# where there is no volume to create the mountpoint.
RUN mkdir -p /run/md

# All three config files - server_config_spot.json, server_config_futures.json,
# venues_config.json - live under user_config/ and are read by that same
# relative path (config.h::ConfigFileForMarket, config/venues_config.h -
# no --config= flag). Copying the directory as a unit, rather than file by
# file, is what keeps the image's layout and the code's relative paths from
# drifting apart the next time a config file is added or renamed. Baked so
# `docker run` needs no mount; compose overlays a bind mount at the same path
# for local editing (docker-compose.yml).
#
# venues_config.json is REQUIRED, not optional. A missing one is a startup
# error rather than a fall-back to built-in defaults, because there are no
# built-in defaults any more - types/venue.h no longer carries the host
# constants. If this COPY is ever dropped, the container fails immediately
# with "venues_config: could not open ..." instead of connecting somewhere
# stale.
COPY user_config/ user_config/

EXPOSE 50051
