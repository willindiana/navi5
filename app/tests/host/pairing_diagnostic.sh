#!/usr/bin/env bash
# Navi5 — synthetic startup failures, no network or native build.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$APP/build/host"
OUT="$(mktemp -d "$APP/build/host/pairing_diagnostic.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer)
clang "${SAN[@]}" -O1 -c "$APP/engine/addons/src/cJSON.c" -I"$APP/engine/addons/include" -o "$OUT/cJSON.o"
# Compile socket option paths for host and PS5; all syscalls replaced by fixtures.
for TARGET in host ps5; do
    FLAGS=()
    if [[ "$TARGET" == ps5 ]]; then FLAGS=(-DEVO_TARGET_PS5); fi
    # shellcheck disable=SC2046
    clang++ "${SAN[@]}" "${FLAGS[@]}" -std=c++20 -fno-rtti -O1 -Wall -Wextra -Werror -pthread \
        -I"$APP/src/subsonic" -I"$APP/engine/addons/include" \
        $(pkg-config --cflags --libs libavutil libcurl) \
        "$APP/src/subsonic/subsonic.cpp" "$APP/tests/host/pairing_diagnostic_test.cpp" \
        "$OUT/cJSON.o" -o "$OUT/test-$TARGET"
    "$OUT/test-$TARGET"
done
clang++ "${SAN[@]}" -std=c++17 -Wall -Wextra -Werror -I"$APP/engine/include" \
    "$APP/tests/host/pairing_ime_test.cpp" -o "$OUT/ime-test"
"$OUT/ime-test"
echo 'PASS: numeric pairing failures, errno capture, first cause, cancellation, entropy fail-closed'
