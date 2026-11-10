#!/usr/bin/env bash
# Navi5 — offline actual-backend continuity. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out="$root/build/host-tests/gapless"
mkdir -p "$out"
ffmpeg -v error -y -f lavfi -i 'aevalsrc=0.25+0.1*sin(2*PI*440*t):s=44100:d=0.613' -ac 2 "$out/A.flac"
for ext in flac mp3 opus m4a; do
    ffmpeg -v error -y -f lavfi -i 'aevalsrc=-0.25+0.1*sin(2*PI*880*t):s=32000:d=0.719' -ac 2 "$out/B.$ext"
done
ffmpeg -v error -y -f lavfi -i 'sine=frequency=330:sample_rate=48000:duration=5' -ac 2 "$out/long.flac"
ffmpeg -v error -y -f lavfi -i 'aevalsrc=0.25:s=48000:d=0.005' -ac 2 "$out/short.flac"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" $(pkg-config --cflags libavformat libavcodec libswresample libavutil libcurl) \
    "$root/app/tests/host/gapless_test.cpp" "$root/app/src/app/audio_player.cpp" "$root/app/src/app/play_queue.cpp" \
    $(pkg-config --libs libavformat libavcodec libswresample libavutil libcurl) -o "$out/gapless-test"
python3 "$root/app/tests/host/gapless_server.py" "$out"
