#!/usr/bin/env bash
# Navi5 — offline real-loopback native listener checks. No console/LAN access.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$APP/build/host"
OUT="$(mktemp -d "$APP/build/host/pairing_listener.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer)
clang "${SAN[@]}" -O1 -c "$APP/engine/addons/src/cJSON.c" -I"$APP/engine/addons/include" -o "$OUT/cJSON.o"
# spawn.h uses exception handling to report std::thread creation failure.
# shellcheck disable=SC2046
clang++ "${SAN[@]}" -std=c++20 -fno-rtti -O1 -Wall -Wextra -Werror -pthread \
    -I"$APP/src/subsonic" -I"$APP/engine/addons/include" \
    $(pkg-config --cflags --libs libavutil libcurl) \
    "$APP/src/subsonic/subsonic.cpp" "$APP/src/subsonic/pairing.cpp" \
    "$APP/src/subsonic/pairing_listener.cpp" "$APP/tests/host/pairing_listener_test.cpp" \
    "$OUT/cJSON.o" -o "$OUT/pairing_listener_test"
"$OUT/pairing_listener_test"
