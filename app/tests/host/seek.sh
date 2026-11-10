#!/usr/bin/env bash
# Navi5 — offline real-backend seek. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out="$root/build/host-tests/seek"
mkdir -p "$out"
# Time-varying content: low tone before 10s, high tone after. Normal trailing moov,
# ~3.75 MB at 128 kb/s; no faststart flag.
ffmpeg -v error -y -f lavfi -i 'aevalsrc=0.3*sin(2*PI*if(lt(t\,10)\,220\,1760)*t):s=44100:d=240' \
    -ac 2 -c:a aac -b:a 128k "$out/track.m4a"
ffmpeg -v error -y -i "$out/track.m4a" -t 30 "$out/track.flac"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" $(pkg-config --cflags libavformat libavcodec libswresample libavutil libcurl) \
    "$root/app/tests/host/seek_test.cpp" "$root/app/src/app/audio_player.cpp" \
    $(pkg-config --libs libavformat libavcodec libswresample libavutil libcurl) -o "$out/seek-test"
python3 "$root/app/tests/host/seek_server.py" "$out"
