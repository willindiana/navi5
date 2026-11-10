#!/usr/bin/env bash
# Navi5 — offline music state checks. No renderer, console or network.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
mkdir -p "$root/build/host-tests"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" "$root/app/src/ui/music_state.cpp" \
    "$root/app/tests/host/music_test.cpp" -o "$root/build/host-tests/music-test"
"$root/build/host-tests/music-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" -I "$root/app/engine/include" "$root/app/src/ui/music_state.cpp" \
    "$root/app/src/app"/i18n*.cpp \
    "$root/app/src/ui/music_screen.cpp" "$root/app/tests/host/music_screen_test.cpp" \
    -o "$root/build/host-tests/music-screen-test"
"$root/build/host-tests/music-screen-test"
# Compact browse footer uses existing native pad geometry, never a literal button name.
grep -Fq 'ui::draw_pad_hints(1750, 1018, {{ui::PadButton::Triangle, T("Fullskjermspiller")}}, 2, 26);' "$root/app/src/main.cpp"
! grep -Fq 'Triangle: åpne spiller' "$root/app/src/main.cpp"
# Retained metadata after Stop must not paint a bar over the idle grid's third row.
grep -Fq '} else if (music.active_session()) {' "$root/app/src/main.cpp"
grep -Fq 'music.session(audio_generation == generation && !song_id.empty() && (audio.state == app::AudioState::playing || audio.state == app::AudioState::paused),' "$root/app/src/main.cpp"
