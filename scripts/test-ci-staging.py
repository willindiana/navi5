#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual staging recipe, linked archive stripping and audited curl pin.

Optional SDK argument also exercises real linked SDK archives, offline.
"""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ci = (ROOT / 'scripts/ci-release.sh').read_text()
app = (ROOT / 'app/scripts/build.sh').read_text()
staging = ci.split('PS5_SYSROOT="$ROOT/build/ci-sdk/target"', 1)[1].split('\nexport PS5_PAYLOAD_SDK=', 1)[0]
staging = 'PS5_SYSROOT="$ROOT/build/ci-sdk/target"' + staging
pin = app.split('python3 - "${archive}" <<\'PY\'\n', 1)[1].split('\nPY', 1)[0]
assert app.index(pin) < app.index('llvm-objcopy "${trace_args[@]}"')
assert ci.index(staging) < ci.index('bash scripts/build.sh --release')
assert 'source "${APP_ROOT}/scripts/sdk-link-inputs.sh"' in app
assert 'for archive in "${SDK_HOMEBREW_ARCHIVES[@]}"' in app
assert '"${CXX_RUNTIME[@]}"' in app and '"${LIBC_ARCHIVE}"' in app
for workflow in ['.github/workflows/release-build.yml', '.forgejo/workflows/release-build.yml']:
    text = (ROOT / workflow).read_text()
    assert 'ca-certificates' in text and 'xz-utils' in text

spec = importlib.util.spec_from_file_location('recipient', ROOT / 'scripts/verify-recipient.py')
recipient = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipient)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    recipe = root / 'app/scripts/sdk-link-inputs.sh'
    recipe.parent.mkdir(parents=True)
    shutil.copyfile(ROOT / 'app/scripts/sdk-link-inputs.sh', recipe)
    inputs = subprocess.check_output(['bash', '-euo', 'pipefail', '-c',
        'PS5_SYSROOT=target HB=target/user/homebrew; source app/scripts/sdk-link-inputs.sh; '
        'printf "%s\\n" "${SDK_HOMEBREW_ARCHIVES[@]}" "${CXX_RUNTIME[@]}" "$LIBC_ARCHIVE"'],
        cwd=root, text=True).splitlines()
    source = root / 'fixture.c'
    source.write_text('int staging_fixture(void) { return 42; }\n')
    obj = root / 'fixture.o'
    subprocess.run(['clang', '--target=x86_64-unknown-freebsd', '-g', '-c', str(source), '-o', str(obj)], check=True)
    fixture = root / 'fixture.a'
    subprocess.run(['llvm-ar', 'rcs', str(fixture), str(obj)], check=True)
    assert b'.debug_info' in subprocess.check_output(['llvm-readelf', '-S', str(fixture)])

    # Always exercise every linked input with real debug sections, even without SDK.
    for sdk in [None] + ([Path(sys.argv[1])] if len(sys.argv) == 2 else []):
        for relative in inputs:
            archive = root / 'build/ci-sdk' / relative
            archive.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(sdk / relative if sdk else fixture, archive)
        curl = root / 'build/ci-sdk/target/user/homebrew/lib/libcurl.a'
        before = hashlib.sha256(curl.read_bytes()).digest()
        unrelated = [root / 'build/ci-sdk' / relative for relative in [
            'target/user/homebrew/lib/libLLVMFileCheck.a',
            'target/user/homebrew/lib/libLLVMDWARFLinker.a',
            'target/lib/libunlinked.a']]
        for archive in unrelated:
            archive.write_bytes(b'invalid archive: must never reach llvm-strip')
        subprocess.run(['bash', '-euo', 'pipefail', '-c', 'ROOT="$PWD"\n' + staging],
                       cwd=root, check=True)
        assert hashlib.sha256(curl.read_bytes()).digest() == before
        for relative in inputs:
            if relative.endswith('/libcurl.a'):
                continue
            archive = root / 'build/ci-sdk' / relative
            assert b'.debug_info' not in subprocess.check_output(['llvm-readelf', '-S', str(archive)])
            if sdk is None:
                assert b' staging_fixture' in subprocess.check_output(['llvm-nm', str(archive)])
        for archive in unrelated:
            assert archive.read_bytes() == b'invalid archive: must never reach llvm-strip'
        result = subprocess.run([sys.executable, '-c', pin, str(curl)], capture_output=True)
        assert (result.returncode == 0) == (sdk is not None), result.stderr.decode()
        if sdk:
            recipient.privacy(curl)
            candidate = root / 'libcurl-trace.a'
            subprocess.run(['llvm-objcopy', '--redefine-sym', 'fcntl=navi5_curl_fcntl',
                            str(curl), str(candidate)], check=True)
            recipient.privacy(candidate)
            assert b'navi5_curl_fcntl' in candidate.read_bytes()

    # Selected archive failure must still stop CI, never leave a silent coverage gap.
    (root / 'build/ci-sdk/target/user/homebrew/lib/libfribidi.a').write_bytes(b'invalid')
    result = subprocess.run(['bash', '-euo', 'pipefail', '-c', 'ROOT="$PWD"\n' + staging],
                            cwd=root, capture_output=True)
    assert result.returncode != 0 and b'libfribidi.a' in result.stderr
print('linked SDK staging, unrelated archive exclusion, curl pin and fail-closed checks passed'
      + (' (real SDK privacy/rewrite passed)' if len(sys.argv) == 2 else ' (fixture)'))
