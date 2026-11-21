#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""CI image inputs, offline: the tag hashes ci/ plus every file the toolchain bake
reads, the build context holds exactly those files, and setup-toolchain.sh uses a
baked toolchain only when its stamp matches the checkout."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, cwd=ROOT, env=None, check=True):
    return subprocess.run(['bash', *args], cwd=cwd, env=env, check=check, text=True, capture_output=True)


# The one list, its expansion and the hash.
listed = [line for line in (ROOT / 'ci/toolchain-inputs').read_text().splitlines()
          if line and not line.startswith('#')]
ci_files = sorted(str(p.relative_to(ROOT)) for p in (ROOT / 'ci').rglob('*') if p.is_file())
inputs = run('scripts/ci-inputs.sh', '--list').stdout.split()
assert inputs == sorted(set(ci_files) | set(listed)), inputs
tracked = set(subprocess.check_output(['git', 'ls-files', '--', *inputs], cwd=ROOT, text=True).split())
assert tracked == set(inputs), 'untracked image input: ' + ', '.join(sorted(set(inputs) - tracked))
lines = ''.join(f'{hashlib.sha256((ROOT / f).read_bytes()).hexdigest()}  {f}\n' for f in inputs)
digest = hashlib.sha256(lines.encode()).hexdigest()
assert run('scripts/ci-inputs.sh', '--hash').stdout.strip() == digest
assert run('scripts/ci-image.sh', '--tag').stdout.strip() == digest[:16]

# Every repository script the bake runs, and everything those reference, is listed.
dockerfile = (ROOT / 'ci/Dockerfile').read_text()
# Repository-relative or "$ROOT/"-relative, never app/scripts/... and the like.
reference = re.compile(r'(?:\$\{?ROOT\}?/|(?<![\w./-]))(scripts/[\w.-]+)')
runs = [block for block in re.split(r'\n(?=[A-Z]+ )', dockerfile) if block.startswith('RUN ')]
pending = {m for block in runs for m in reference.findall(block)}
# Named in messages and comments only: tags and pushes, never runs inside the bake.
NOT_INPUTS = {'scripts/ci-image.sh'}
needed = set()
while pending:
    name = pending.pop()
    if name in needed or name in NOT_INPUTS or not (ROOT / name).is_file():
        continue
    needed.add(name)
    pending |= set(reference.findall((ROOT / name).read_text()))
assert {'scripts/setup-toolchain.sh', 'scripts/build-av1.sh', 'scripts/download.sh',
        'scripts/prepare-ci-inputs.py', 'scripts/source-inputs.json', 'scripts/ci-inputs.sh'} <= needed
missing = needed - set(listed) - NOT_INPUTS
assert not missing, 'bake input missing from ci/toolchain-inputs: ' + ', '.join(sorted(missing))

# The Dockerfile copies exactly the listed files (plus ci/), and the image names
# its toolchain only after the last bake step.
copied = set()
for line in dockerfile.splitlines():
    if line.startswith('COPY '):
        assert '--from' not in line, line
        copied |= set(line.split()[1:-1])
assert copied - {'ci/'} == set(listed), sorted(copied ^ set(listed))
assert 'ci/' in copied
env = dockerfile.index('ENV NAVI5_BAKED_TOOLCHAIN=/opt/navi5/toolchain')
assert env > dockerfile.rindex('\nRUN ') and dockerfile.rstrip().endswith('ENV NAVI5_BAKED_TOOLCHAIN=/opt/navi5/toolchain')
assert 'scripts/ci-inputs.sh --hash > toolchain/.navi5-inputs' in dockerfile
assert 'python3 -B scripts/prepare-ci-inputs.py --fetch-only' in dockerfile
# Build context: a staging copy of exactly the hashed inputs, never ci/ or the repo root.
image = (ROOT / 'scripts/ci-image.sh').read_text()
assert 'bash scripts/ci-inputs.sh --hash | cut -c1-16' in image
assert 'bash scripts/ci-inputs.sh --list | while' in image
assert 'docker build --pull -t "$image" -f "$context/ci/Dockerfile" "$context"' in image
assert len(re.findall(r'docker build', image)) == 1

# setup-toolchain.sh with a baked toolchain: stamp checked, linked, never mixed.
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    root = temp / 'checkout'
    for name in inputs:
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, root / name)
    baked = temp / 'baked'
    for name in ['ps5-payload-sdk/bin/prospero-clang', 'ps5-payload-sdk/target/user/homebrew/.pacbrew-v0.39',
                 'host-zlib/lib/libz.a', 'av1-prefix/lib/libdav1d.a', 'av1-prefix/lib/libavcodec.a',
                 'source-cache/fixture.tar.gz']:
        (baked / name).parent.mkdir(parents=True, exist_ok=True)
        (baked / name).write_bytes(b'')
    stamp = baked / '.navi5-inputs'
    env = {k: v for k, v in os.environ.items() if k != 'NAVI5_BAKED_TOOLCHAIN'}
    # No network and no host tools needed: curl that fails proves nothing is fetched.
    fake = temp / 'bin'
    fake.mkdir()
    (fake / 'curl').write_text('#!/bin/sh\necho "unexpected download" >&2\nexit 9\n')
    (fake / 'curl').chmod(0o755)
    env['PATH'] = f'{fake}:{env["PATH"]}'
    env['NAVI5_BAKED_TOOLCHAIN'] = str(baked)
    setup = ['scripts/setup-toolchain.sh']
    link = root / 'toolchain'

    def fails(message):
        result = run(*setup, cwd=root, env=env, check=False)
        assert result.returncode != 0 and message in result.stderr, (result.stdout, result.stderr)

    fails('no input stamp')
    assert not link.exists() and not link.is_symlink()
    stamp.write_text('0' * 64 + '\n')
    fails('this checkout needs ' + digest[:16])
    assert not link.is_symlink()
    stamp.write_text(digest + '\n')
    (baked / 'host-zlib/lib/libz.a').unlink()
    fails('missing host-zlib/lib/libz.a')
    (baked / 'host-zlib/lib/libz.a').write_bytes(b'')
    link.mkdir()
    fails('already exists in the checkout')
    link.rmdir()
    for _ in range(2):  # links once, then accepts its own link
        result = run(*setup, cwd=root, env=env)
        assert 'using baked toolchain' in result.stdout and digest[:16] in result.stdout
        assert link.is_symlink() and os.readlink(link) == str(baked)
    # An edited input in the checkout no longer matches the baked stamp.
    (root / 'scripts/source-inputs.json').write_text((ROOT / 'scripts/source-inputs.json').read_text() + '\n')
    fails('this checkout needs')
    link.unlink()
    os.symlink(temp, link)
    shutil.copyfile(ROOT / 'scripts/source-inputs.json', root / 'scripts/source-inputs.json')
    fails('toolchain/ links elsewhere')
    # --env stays a pure printer, baked or not.
    assert 'PS5_PAYLOAD_SDK=' in run(*setup, '--env', cwd=root, env=env).stdout
print('CI image inputs, staged build context and baked toolchain stamp checks passed')
