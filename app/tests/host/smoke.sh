#!/usr/bin/env bash
# Navi5 — offline integration checks. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out="$root/build/host-tests/smoke"
mkdir -p "$out"
clang -O1 -fsanitize=address,undefined -I "$root/app/engine/addons/include" -c "$root/app/engine/addons/src/cJSON.c" -o "$out/cJSON.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" -I "$root/app/engine/addons/include" \
    $(pkg-config --cflags libavformat libavcodec libswresample libavutil libcurl) \
    "$root/app/tests/host/smoke_test.cpp" "$root/app/src/app/smoke_session.cpp" \
    "$root/app/src/app/recent_contexts.cpp" "$root/app/src/app/audio_player.cpp" "$root/app/src/app/play_queue.cpp" "$root/app/src/ui/music_state.cpp" "$root/app/src/subsonic/subsonic.cpp" "$out/cJSON.o" \
    $(pkg-config --libs libavformat libavcodec libswresample libavutil libcurl) -o "$out/smoke-test"
"$out/smoke-test"
