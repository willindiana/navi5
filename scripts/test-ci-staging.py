#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual staging command and actual app curl pin before symbol rewrite.

Optional SDK argument exercises real audited curl and debug stripping, offline.
Without SDK, a tiny fixture exercises exclusion and pin rejection only.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ci = (ROOT / 'scripts/ci-release.sh').read_text()
app = (ROOT / 'app/scripts/build.sh').read_text()
staging = next(line for line in ci.splitlines() if line.startswith('find build/ci-sdk/target '))
pin = app.split('python3 - "${archive}" <<\'PY\'\n', 1)[1].split('\nPY', 1)[0]
assert app.index(pin) < app.index('llvm-objcopy "${trace_args[@]}"')
assert ci.index(staging) < ci.index('bash scripts/build.sh --release')
for workflow in ['.github/workflows/release-build.yml', '.forgejo/workflows/release-build.yml']:
    text = (ROOT / workflow).read_text()
    assert 'ca-certificates' in text and 'xz-utils' in text

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    lib = root / 'build/ci-sdk/target/user/homebrew/lib'
    lib.mkdir(parents=True)
    curl = lib / 'libcurl.a'
    real = len(sys.argv) == 2
    if real:
        sdk = Path(sys.argv[1])
        shutil.copyfile(sdk / 'target/user/homebrew/lib/libcurl.a', curl)
        shutil.copyfile(sdk / 'target/user/homebrew/lib/libfribidi.a', lib / 'libfribidi.a')
    else:
        curl.write_bytes(b'curl fixture, never strip')
    before = hashlib.sha256(curl.read_bytes()).digest()
    subprocess.run(['bash', '-euo', 'pipefail', '-c', staging], cwd=root, check=True)
    assert hashlib.sha256(curl.read_bytes()).digest() == before
    result = subprocess.run([sys.executable, '-c', pin, str(curl)], capture_output=True)
    assert (result.returncode == 0) == real, result.stderr.decode()
    # Same marker set used by actual recipient checker, not a new weaker list.
    import importlib.util
    spec = importlib.util.spec_from_file_location('recipient', ROOT / 'scripts/verify-recipient.py')
    recipient = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(recipient)
    recipient.privacy(curl)
    if real:
        # Symbol rewrite itself must keep candidate recipient-safe too.
        candidate = root / 'libcurl-trace.a'
        subprocess.run(['llvm-objcopy', '--redefine-sym', 'fcntl=navi5_curl_fcntl',
                        str(curl), str(candidate)], check=True)
        recipient.privacy(candidate)
        assert b'navi5_curl_fcntl' in candidate.read_bytes()
print('actual staging command, curl pin ordering and privacy passed' + (' (real SDK)' if real else ' (fixture)'))
