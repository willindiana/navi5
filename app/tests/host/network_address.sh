#!/usr/bin/env bash
# Navi5 — private IPv4 discovery with injected getifaddrs; no host enumeration.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$APP/build/host"
OUT="$(mktemp -d "$APP/build/host/network_address.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
clang++ -fsanitize=address,undefined -fno-omit-frame-pointer -std=c++17 -O1 -Wall -Wextra -Werror \
    -I"$APP/src/platform" "$APP/src/platform/network_address.cpp" \
    "$APP/tests/host/network_address_test.cpp" -o "$OUT/test"
"$OUT/test"
echo 'PASS: private IPv4 filter, RFC1918 bounds, dedup, cleanup, API failure'
