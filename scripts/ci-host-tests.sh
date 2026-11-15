#!/usr/bin/env bash
# Navi5 — offline host checks for login, saved credentials, key issuance and
# DEV/PROD identity. No console, no server, no network after dependencies.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   bash scripts/ci-host-tests.sh
# Needs clang, pkg-config, python3, openssl and dev packages for libcurl and
# FFmpeg (libavformat, libavcodec, libswresample, libavutil).
set -euo pipefail
export LC_ALL=C PYTHONDONTWRITEBYTECODE=1
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
failed=()
step() {
    local name="$1"; shift
    local log; log="$(mktemp)"
    if "$@" >"$log" 2>&1; then
        echo "ok    $name"
    else
        echo "FAIL  $name"; tail -40 "$log"; failed+=("$name")
    fi
    rm -f "$log"
}
# Credential storage, key issuance, Subsonic errors, pairing form and worker.
step saved_login       bash app/tests/host/saved_login.sh
step api_key           bash app/tests/host/api_key.sh
step subsonic          env NAVI5_SANITIZE=1 bash app/tests/host/subsonic.sh
step pairing           bash app/tests/host/pairing.sh
step pairing_listener  bash app/tests/host/pairing_listener.sh
step smoke_worker      bash app/tests/host/smoke.sh
step transport_tls     python3 -B app/tests/host/transport.py
# DEV/PROD identity, deploy guards and dev-login staging.
step package_identity  python3 -B app/tests/host/smoke_package.py
step deploy_guards     python3 -B scripts/test_deploy.py
step stage_dev_login   python3 -B app/tests/host/stage_dev_login.py
step workflow_policy   python3 -B scripts/test-workflow-policy.py
step pr_test_note      python3 -B scripts/test-pr-test-note.py
# v1.0: queue model and worker edits (smoke_worker above), controls, login/QR view, packaging.
step queue             bash app/tests/host/queue.sh
step controls_ui       bash app/tests/host/music.sh
step controller_map    bash app/tests/host/input.sh
step pairing_view      bash app/tests/host/pairing_view.sh
step release_package   python3 -B scripts/test-package-release.py
step tag_source        python3 -B scripts/test-verify-tag-source.py
if (( ${#failed[@]} )); then
    echo "FAILED: ${failed[*]}" >&2
    exit 1
fi
echo "PASS: host checks"
