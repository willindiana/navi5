#!/usr/bin/env bash
# Navi5 — offline audio decode checks. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out="$root/build/host-tests/audio"
mkdir -p "$out"
for ext in mp3 flac aac opus m4a; do
    ffmpeg -v error -y -f lavfi -i 'sine=frequency=440:sample_rate=44100:duration=0.25' "$out/tone.$ext"
done
# Incompressible stereo FLAC exceeds old generic GET's 16MiB ceiling.
ffmpeg -v error -y -f lavfi -i 'aevalsrc=random(0)*2-1|random(1)*2-1:sample_rate=48000:duration=120' \
    -ac 2 -sample_fmt s32 "$out/large.flac"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" $(pkg-config --cflags libavformat libavcodec libswresample libavutil libcurl) \
    "$root/app/tests/host/audio_test.cpp" "$root/app/src/app/audio_player.cpp" \
    $(pkg-config --libs libavformat libavcodec libswresample libavutil libcurl) \
    -o "$out/audio-test"
python3 "$root/app/tests/host/audio_server.py" "$out"
