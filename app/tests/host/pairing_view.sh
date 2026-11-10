#!/usr/bin/env bash
# Navi5 — offline pairing view checks; no listener, LAN or console.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
mkdir -p "$root/build/host-tests"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" -c "$root/app/src/qrcodegen.c" -o "$root/build/host-tests/pairing-view-qr.o"
for mode in release dev; do
    flags=()
    if [[ "$mode" == dev ]]; then flags=(-DNAVI5_DEV_TOOLS=1); fi
    "${CXX:-c++}" "${flags[@]}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I "$root/app/src" "$root/app/src/ui/pairing_view.cpp" \
        "$root/app/tests/host/pairing_view_test.cpp" "$root/build/host-tests/pairing-view-qr.o" \
        -o "$root/build/host-tests/pairing-view-test-$mode"
    "$root/build/host-tests/pairing-view-test-$mode"
done
echo 'PASS: pairing consent, actions, stale results, QR rendering, expiry, generic status'
