#!/usr/bin/env bash
# Navi5 — offline play queue checks.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
mkdir -p "$root/build/host-tests"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/app/src" "$root/app/tests/host/queue_test.cpp" \
    "$root/app/src/app/play_queue.cpp" -o "$root/build/host-tests/queue-test"
"$root/build/host-tests/queue-test"
echo 'PASS: queue navigation, repeat, seeded shuffle, duplicate IDs and queue edits'
