# Navi5 — CI job image: the runner's default job image plus everything the host
# checks and the RELEASE build install, and the PS5 toolchain baked from pinned
# inputs. Jobs use it by content tag; it is rebuilt and pushed only when a file in
# ci/ or a file ci/toolchain-inputs names changes (scripts/ci-image.sh). The
# build context holds exactly those files.
# SPDX-License-Identifier: GPL-3.0-or-later
FROM ghcr.io/catthehacker/ubuntu:act-24.04@sha256:7bf3a8afc2bbfe7fdaf4b9d2716cb2f3634fd21e4f38e26aa17a12cfda80d721
# Throwaway layers: skip dpkg's per-file syncs, which are very slow on the runner's storage.
RUN echo force-unsafe-io > /etc/dpkg/dpkg.cfg.d/90-ci-unsafe-io \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential clang clang-18 lld-18 llvm-18 libclang-rt-18-dev \
      pkg-config python3 python3-venv openssl ca-certificates curl git \
      xz-utils unzip zip nasm meson ninja-build cmake \
      libcurl4-openssl-dev libavformat-dev libavcodec-dev libswresample-dev libavutil-dev \
 && rm -f /etc/dpkg/dpkg.cfg.d/90-ci-unsafe-io \
 && rm -rf /var/lib/apt/lists/*

# The PS5 toolchain as scripts/setup-toolchain.sh builds it in a checkout, at
# /opt/navi5/toolchain: payload SDK with the pacbrew sysroot merged, host zlib and
# the AV1 prefix (dav1d + FFmpeg), all checksum-pinned. Its own layer, so a change
# to the source pins below does not rebuild it on a daemon that kept the cache.
COPY scripts/setup-toolchain.sh scripts/download.sh scripts/build-av1.sh /opt/navi5/scripts/
RUN cd /opt/navi5 \
 && LLVM_CONFIG=llvm-config-18 bash scripts/setup-toolchain.sh \
 && rm -rf toolchain/dl build
# The pinned third-party source archives the source-delivery gate packs, fetched
# and checksum-verified once. The RELEASE build verifies every one again.
COPY scripts/prepare-ci-inputs.py scripts/source-inputs.json /opt/navi5/scripts/
RUN cd /opt/navi5 && python3 -B scripts/prepare-ci-inputs.py --fetch-only
# Stamp: the input hash this toolchain was built from, computed from the build
# context itself. setup-toolchain.sh uses the toolchain only when the checkout's
# hash matches it.
COPY ci/ /opt/navi5/ci/
COPY scripts/ci-inputs.sh /opt/navi5/scripts/
RUN cd /opt/navi5 \
 && bash scripts/ci-inputs.sh --hash > toolchain/.navi5-inputs \
 && rm -rf ci scripts
ENV NAVI5_BAKED_TOOLCHAIN=/opt/navi5/toolchain
