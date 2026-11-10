#!/usr/bin/env bash
# Navi5 — offline observation ABI/bounds/release check.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -pthread \
    -I"$ROOT/src" "$ROOT/tests/host/socket_trace.cpp" -o "$TMP/check"
"$TMP/check"
clang++ -std=c++17 -c "$ROOT/src/socket_trace.cpp" -o "$TMP/release.o"
if nm "$TMP/release.o" | grep -E 'navi5_trace_|socket_trace'; then
    echo 'FAIL: release contains observer'; exit 1
fi
clang++ -std=c++17 -DEVO_TARGET_PS5 -c "$ROOT/src/curl_fcntl.cpp" -o "$TMP/fix.o"
nm "$TMP/fix.o" | grep -q navi5_curl_fcntl
if nm "$TMP/fix.o" | grep -E 'socket_trace|navi5_trace_'; then
    echo 'FAIL: production fix contains observer'; exit 1
fi
# Release: sockopt fix present, observer absent.
clang++ -std=c++17 -DEVO_TARGET_PS5 -Wall -Wextra -fsanitize=address,undefined \
    "$ROOT/tests/host/curl_sockopt.cpp" -o "$TMP/sockopt"
"$TMP/sockopt"
printf '#include "curl_sockopt.h"\nvoid *keep = (void *)jf::curl_sockopt;\n' |
    clang++ -std=c++17 -DEVO_TARGET_PS5 -I"$ROOT/src" -x c++ -c - -o "$TMP/sockopt.o"
nm "$TMP/sockopt.o" | grep -qE ' U _?setsockopt$'
if nm "$TMP/sockopt.o" | grep -E 'socket_trace|navi5_trace_'; then
    echo 'FAIL: release sockopt contains observer'; exit 1
fi
echo 'PASS: exact fallback, non-socket rejection, ABI, errno, bounds, production fix without observer, SO_NBIO fail closed, ring bounds'
