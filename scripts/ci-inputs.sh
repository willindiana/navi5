#!/usr/bin/env bash
# Navi5 — the inputs of the CI image and its baked PS5 toolchain, and their hash.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   bash scripts/ci-inputs.sh --list   # every file in ci/ plus those ci/toolchain-inputs names
#   bash scripts/ci-inputs.sh --hash   # sha256 over those paths and their contents
#
# scripts/ci-image.sh tags the image with this hash; the image records it in the
# baked toolchain, and scripts/setup-toolchain.sh uses that toolchain only when
# the checkout's hash still matches.
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

list() {
    local line
    {
        find ci -type f
        while IFS= read -r line || [[ -n "$line" ]]; do
            [[ -z "$line" || "$line" == \#* ]] && continue
            [[ "$line" =~ ^[A-Za-z0-9_./-]+$ && "$line" != /* && "$line" != *..* && -f "$line" && ! -L "$line" ]] ||
                { echo "ci/toolchain-inputs: not a regular repository file: $line" >&2; exit 1; }
            echo "$line"
        done < ci/toolchain-inputs
    } | sort -u
}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

case "${1:-}" in
    --list) list ;;
    --hash)
        files="$(list)"
        # One "digest  path" line per input, then the digest of those lines.
        while IFS= read -r f; do sha256 "$f"; done <<<"$files" | sha256 | cut -c1-64 ;;
    *) echo "usage: $0 --list | --hash" >&2; exit 2 ;;
esac
