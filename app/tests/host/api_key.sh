#!/usr/bin/env bash
# Navi5 — offline API key issuance checks; mock transport, fake credentials, no server.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "${APP}/build/host"
OUT="$(mktemp -d "${APP}/build/host/api_key.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer)
clang "${SAN[@]}" -O1 -w -c "$APP/engine/addons/src/cJSON.c" -I"$APP/engine/addons/include" -o "$OUT/cJSON.o"
# shellcheck disable=SC2046
clang++ "${SAN[@]}" -std=c++20 -fno-exceptions -fno-rtti -O1 -Wall -Wextra -Werror \
    -I"$APP/src/subsonic" -I"$APP/engine/addons/include" $(pkg-config --cflags --libs libavutil) \
    "$APP/src/subsonic/subsonic.cpp" "$APP/src/subsonic/api_key.cpp" \
    "$APP/tests/host/api_key_test.cpp" "$OUT/cJSON.o" -o "$OUT/api_key_test"
"$OUT/api_key_test"
echo 'PASS: key format/uniqueness, Navidrome-only issuance, login/create/verify order, header-safe token, rollback on failed verify'
