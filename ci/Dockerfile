# Navi5 — CI job image: the runner's default job image plus everything the host
# checks and the RELEASE build install. Jobs use it by content tag; it is rebuilt
# and pushed only when a file in ci/ changes (scripts/ci-image.sh).
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
