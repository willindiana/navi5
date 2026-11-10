#!/usr/bin/env bash
# Navi5 — trusted manual builds only; never creates tags or releases.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[[ -z "$(git status --porcelain --untracked-files=normal)" ]] || { echo 'Commit reviewed source first' >&2; exit 1; }
[[ ! -e app/build && ! -e build/ci-release && ! -e build/ci-source ]] || { echo 'Fresh build directories required; preserve previous artifacts elsewhere' >&2; exit 1; }
# Never inherit local development destinations, overrides or remote logging.
unset JELLY5_REMOTE_LOG JELLY5_LOG_HOST JELLY5_LOG_PORT NUVIO_BUILD_IMAGE EVO_ROOT JELLY5_AV1_PREFIX
export NAVI5_DEV_TOOLS=0 JELLY5_AV1=1
bash scripts/setup-toolchain.sh
eval "$(bash scripts/setup-toolchain.sh --env)"
command -v llvm-readelf >/dev/null
bash app/tests/host/input.sh
bash app/tests/host/shell.sh
revision="$(git rev-parse HEAD)"
(cd app && bash scripts/build.sh --release)
[[ "$revision" == "$(git rev-parse HEAD)" && -z "$(git status --porcelain --untracked-files=normal)" ]] || { echo 'Source changed during build' >&2; exit 1; }
printf '%s\n' "$revision" > app/build/source-revision.txt
python3 scripts/check-ci-release.py
# Completion needs owner-prepared modified library/source, never a fabricated label.
: "${NAVI5_MODIFIED_FRIBIDI:?Set path to reviewed modified FriBidi archive}"
: "${NAVI5_MODIFIED_FRIBIDI_SOURCE:?Set path to matching modified fribidi.c}"
python3 scripts/prepare-relink.py app/build/relink.tar build/recipient-relink.tar
python3 scripts/verify-recipient.py build/recipient-relink.tar "$NAVI5_MODIFIED_FRIBIDI" build/recipient-verification
# Offline source assembly: no binary-only fallback, no automatic source downloads.
python3 scripts/assemble-source.py --cache toolchain/source-cache --output build/ci-source \
  --relink app/build/relink.tar --verification build/recipient-verification/verification.json \
  --modified-library "$NAVI5_MODIFIED_FRIBIDI" --modified-source "$NAVI5_MODIFIED_FRIBIDI_SOURCE"
python3 scripts/check-ci-release.py --delivery
