#!/usr/bin/env bash
# Navi5 — bounded private-file parsing regression.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
APP="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$APP/build/host"
mkdir -p "$OUT"
clang -Wno-deprecated-declarations -fsanitize=address,undefined -c "$APP/engine/addons/src/cJSON.c" -I"$APP/engine/addons/include" -o "$OUT/dev-json.o"
for test in dev_login temporary_login reader_check; do
    clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$APP/src" -I"$APP/engine/addons/include" "$APP/tests/host/$test.cpp" "$OUT/dev-json.o" -o "$OUT/$test-test"
    "$OUT/$test-test"
done
