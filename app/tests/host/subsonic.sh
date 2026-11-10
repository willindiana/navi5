#!/usr/bin/env bash
# Builds and runs the Subsonic client host test. Offline fixture checks always
# run; NAVI5_LIVE=1 adds a read-only smoke against the public Navidrome demo.
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${APP}/build/host"
mkdir -p "${OUT}"
FF="$(pkg-config --cflags --libs libavutil)"
SAN=()
if [[ "${NAVI5_SANITIZE:-0}" == "1" ]]; then
    SAN=(-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer)
fi
clang "${SAN[@]}" -O1 -c "${APP}/engine/addons/src/cJSON.c" -I"${APP}/engine/addons/include" -o "${OUT}/cJSON.o"
# shellcheck disable=SC2086
clang++ "${SAN[@]}" -std="${NAVI5_CXX_STD:-c++20}" -fno-exceptions -fno-rtti -O1 -Wall -Wextra -Werror -I"${APP}/src/subsonic" -I"${APP}/engine/addons/include" \
    ${FF} "${APP}/src/subsonic/subsonic.cpp" "${APP}/tests/host/subsonic_test.cpp" \
    "${APP}/tests/host/jf_http_curl.cpp" -I"${APP}/src/jf" \
    "${OUT}/cJSON.o" -lcurl -o "${OUT}/subsonic_test"
"${OUT}/subsonic_test"
