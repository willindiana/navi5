#!/usr/bin/env bash
# Navi5 — offline real-backend orchestration. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out="$root/build/host-tests/playback-audio"
mkdir -p "$out"
for track in A B; do
    frequency=440
    [[ "$track" == B ]] && frequency=880
    ffmpeg -v error -y -f lavfi -i "sine=frequency=$frequency:sample_rate=44100:duration=0.6" "$out/$track.flac"
done
clang -O1 -fsanitize=address,undefined -I "$root/app/engine/addons/include" \
    -c "$root/app/engine/addons/src/cJSON.c" -o "$out/cJSON.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" -I "$root/app/engine/addons/include" \
    $(pkg-config --cflags libavformat libavcodec libswresample libavutil libcurl) \
    "$root/app/tests/host/playback_audio_test.cpp" "$root/app/src/app/smoke_session.cpp" "$root/app/src/app/recent_contexts.cpp" \
    "$root/app/src/app/audio_player.cpp" "$root/app/src/app/play_queue.cpp" \
    "$root/app/src/ui/music_state.cpp" "$root/app/src/subsonic/subsonic.cpp" "$out/cJSON.o" \
    $(pkg-config --libs libavformat libavcodec libswresample libavutil libcurl) \
    -o "$out/playback-audio-test"
python3 "$root/app/tests/host/playback_audio_server.py" "$out"
