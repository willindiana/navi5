#!/usr/bin/env bash
# Navi5 — offline generic shell checks.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
mkdir -p "$root/build/host-tests"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" "$root/app/tests/host/shell_test.cpp" -o "$root/build/host-tests/shell-test"
"$root/build/host-tests/shell-test"
echo 'PASS: JSON number bounds, frame-rate-independent animation and easing'
