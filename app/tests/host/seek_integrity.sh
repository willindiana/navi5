#!/usr/bin/env bash
# Navi5 — offline seek integrity. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out="$root/build/host-tests/seek-integrity"
mkdir -p "$out"
for ext in flac mp3 opus m4a; do
    ffmpeg -v error -y -f lavfi -i 'aevalsrc=0.3*sin(2*PI*(220*t+7*t*t)):s=44100:d=3.17' -ac 2 "$out/track.$ext"
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" $(pkg-config --cflags libavformat libavcodec libswresample libavutil libcurl) \
    "$root/app/tests/host/seek_integrity_test.cpp" \
    $(pkg-config --libs libavformat libavcodec libswresample libavutil libcurl) -o "$out/seek-integrity-test"
python3 "$root/app/tests/host/seek_integrity_server.py" "$out"
