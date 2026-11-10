#!/usr/bin/env bash
# Navi5 — trusted manual builds only; never creates tags or releases.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[[ -z "$(git status --porcelain --untracked-files=normal)" ]] || { echo 'Commit reviewed source first' >&2; exit 1; }
[[ ! -e app/build && ! -e build/ci-release && ! -e build/ci-delivery && ! -e build/ci-source && ! -e build/ci-sdk && ! -e build/ci-ffmpeg && ! -e build/ci-modified-fribidi && ! -e build/recipient-relink.tar && ! -e build/recipient-verification ]] || { echo 'Fresh build directories required; preserve previous artifacts elsewhere' >&2; exit 1; }
# Never inherit local development destinations, overrides or remote logging.
unset JELLY5_REMOTE_LOG JELLY5_LOG_HOST JELLY5_LOG_PORT NUVIO_BUILD_IMAGE EVO_ROOT JELLY5_AV1_PREFIX
export NAVI5_DEV_TOOLS=0 JELLY5_AV1=1
python3 -B scripts/test-package-release.py
python3 -B scripts/test-ci-release.py
python3 -B scripts/test-prepare-ci-inputs.py
bash scripts/setup-toolchain.sh
eval "$(bash scripts/setup-toolchain.sh --env)"
command -v llvm-readelf >/dev/null
bash app/tests/host/input.sh
bash app/tests/host/shell.sh
python3 -B scripts/prepare-ci-inputs.py --fetch-only
bash scripts/build-release-ffmpeg.sh build/ci-ffmpeg
export JELLY5_AV1_PREFIX="$ROOT/build/ci-ffmpeg/av1-prefix"
# Strip debug sections only on private delivery copy; retain symbols for relinking.
mkdir -p build/ci-sdk
cp -R toolchain/ps5-payload-sdk/. build/ci-sdk/
# Use actual linker inputs, not unrelated SDK archives (including LLVM bitcode).
# Keep audited curl bytes intact: app build verifies pin before symbol rewrite.
PS5_SYSROOT="$ROOT/build/ci-sdk/target" HB="$ROOT/build/ci-sdk/target/user/homebrew"
source app/scripts/sdk-link-inputs.sh
for archive in "${SDK_HOMEBREW_ARCHIVES[@]}" "${CXX_RUNTIME[@]}" "$LIBC_ARCHIVE"; do
    [[ "${archive##*/}" == libcurl.a ]] || llvm-strip --strip-debug "$archive"
done
export PS5_PAYLOAD_SDK="$ROOT/build/ci-sdk"
export PATH="$PS5_PAYLOAD_SDK/bin:$PATH"
python3 -B scripts/test-ci-staging.py "$PS5_PAYLOAD_SDK"
revision="$(git rev-parse HEAD)"
(cd app && bash scripts/build.sh --release)
[[ "$revision" == "$(git rev-parse HEAD)" && -z "$(git status --porcelain --untracked-files=normal)" ]] || { echo 'Source changed during build' >&2; exit 1; }
printf '%s\n' "$revision" > app/build/source-revision.txt
python3 scripts/check-ci-release.py
# Shared fresh-run preparation; pins verified before use, replacement built from source.
python3 -B scripts/prepare-ci-inputs.py --offline
NAVI5_MODIFIED_FRIBIDI="$ROOT/build/ci-modified-fribidi/fribidi-1.0.15/lib/.libs/libfribidi.a"
NAVI5_MODIFIED_FRIBIDI_SOURCE="$ROOT/build/ci-modified-fribidi/fribidi-1.0.15/lib/fribidi.c"
python3 scripts/prepare-relink.py app/build/relink.tar build/recipient-relink.tar
python3 scripts/verify-recipient.py build/recipient-relink.tar "$NAVI5_MODIFIED_FRIBIDI" build/recipient-verification
# Offline source assembly: no binary-only fallback, no automatic source downloads.
python3 scripts/assemble-source.py --cache toolchain/source-cache --output build/ci-source \
  --relink app/build/relink.tar --verification build/recipient-verification/verification.json \
  --modified-library "$NAVI5_MODIFIED_FRIBIDI" --modified-source "$NAVI5_MODIFIED_FRIBIDI_SOURCE"
python3 scripts/check-ci-release.py --delivery
# Both host workflows collect ci-release; preserve full validated delivery separately.
mv build/ci-release build/ci-delivery
python3 -B scripts/package-release.py "app/build/app/Navi5-$(python3 -c 'import json; print(json.load(open("app/sce_sys/param.json"))["contentVersion"])').zip" build/ci-delivery build/ci-release
