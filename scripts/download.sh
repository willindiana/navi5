#!/usr/bin/env bash
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
# Shared checksum-pinned download/cache helper. Caller sets DL.
sha256_ok() { # sha file
    if command -v sha256sum >/dev/null 2>&1; then
        echo "$1  $2" | sha256sum -c - >/dev/null 2>&1
    else
        echo "$1  $2" | shasum -a 256 -c - >/dev/null 2>&1
    fi
}

fetch() { # url sha file
    local url="$1" sha="$2" out="${DL}/$3"
    mkdir -p "${DL}"
    [[ ! -L "$out" && ! -L "$out.part" ]] || { echo 'Symlink cache entry rejected' >&2; return 1; }
    if [[ ! -f "$out" ]] || ! sha256_ok "$sha" "$out"; then
        echo "==> downloading $3"
        curl -fSL --proto '=https' --proto-redir '=https' --connect-timeout 30 --max-time 300 \
            --retry 3 --retry-delay 2 --retry-max-time 900 -o "$out.part" "$url" || return 1
        sha256_ok "$sha" "$out.part" || { echo "checksum mismatch: $3" >&2; return 1; }
        mv "$out.part" "$out"
    fi
    sha256_ok "$sha" "$out" || { echo "checksum mismatch: $3" >&2; return 1; }
}
