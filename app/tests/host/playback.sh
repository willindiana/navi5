#!/usr/bin/env bash
# Navi5 — offline orchestration checks. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/navi5-playback.XXXXXX")
trap 'rm -rf "$out"' EXIT
clang -O1 -fsanitize=address,undefined -I "$root/app/engine/addons/include" -c "$root/app/engine/addons/src/cJSON.c" -o "$out/cJSON.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" -I "$root/app/engine/addons/include" $(pkg-config --cflags libavutil) \
    "$root/app/tests/host/playback_test.cpp" "$root/app/src/app/smoke_session.cpp" "$root/app/src/app/recent_contexts.cpp" \
    "$root/app/src/app/play_queue.cpp" "$root/app/src/ui/music_state.cpp" \
    "$root/app/src/subsonic/subsonic.cpp" "$out/cJSON.o" $(pkg-config --libs libavutil) -o "$out/playback-test"
"$out/playback-test"
