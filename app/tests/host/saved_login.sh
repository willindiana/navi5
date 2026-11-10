#!/usr/bin/env bash
# Navi5 — offline saved-login storage checks; temp dir, fake credentials, no console.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out="$root/build/host-tests"
mkdir -p "$out"
"${CC:-cc}" -O1 -w -fsanitize=address,undefined -I "$root/app/engine/addons/include" \
    -c "$root/app/engine/addons/src/cJSON.c" -o "$out/saved-login-cJSON.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    -I "$root/app/src" -I "$root/app/engine/addons/include" \
    "$root/app/src/app/saved_login.cpp" "$root/app/tests/host/saved_login_test.cpp" \
    "$out/saved-login-cJSON.o" -o "$out/saved-login-test"
"$out/saved-login-test"
echo 'PASS: saved login roundtrip, 0600/0700, atomic replace, 8192 bound, junk/NUL/oversize/FIFO/symlink rejected, forget+tmp'
