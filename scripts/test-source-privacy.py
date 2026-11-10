#!/usr/bin/env python3
# Navi5 — source privacy regression; no native build or network needed.
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import io
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tarfile
import tempfile
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
check = runpy.run_path(str(ROOT / 'scripts/check-ci-release.py'))['check_source_privacy']


def rejects(call, message):
    try:
        call()
    except ValueError as error:
        assert str(error) == message
    else:
        raise AssertionError('unsafe source accepted')


def archive(path, name, data=b'', link=''):
    with tarfile.open(path, 'w:gz') as tar:
        member = tarfile.TarInfo(name)
        if link:
            member.type = tarfile.SYMTYPE
            member.linkname = link
        else:
            member.size = len(data)
        tar.addfile(member, io.BytesIO(data))


with tempfile.TemporaryDirectory() as directory, patch.object(Path, 'home', return_value=Path('/root')):
    source = Path(directory)
    upstream = source / 'libpsl-fixture.tar.gz'
    # Exact public URL shape in pinned libpsl-0.21.5/list/public_suffix_list.dat.
    archive(upstream, 'libpsl/list/public_suffix_list.dat', b'// https://www.iana.org/domains/' + b'root/db/ac.html\n')
    pins = [{'file': upstream.name, 'sha256': hashlib.sha256(upstream.read_bytes()).hexdigest()}]
    check(source, pins)
    upstream.write_bytes(upstream.read_bytes() + b'tampered')
    rejects(lambda: check(source, pins), 'source input hash mismatch: ' + upstream.name)
    upstream.unlink()
    payload = source / 'navi5.tar.gz'
    for data in (b'/' + b'root/private/build.o', b'"/' + b'root/private/build.o"', b'\x00/' + b'root/private/build.o'):
        archive(payload, 'app/source.c', data)
        rejects(lambda: check(source, []), 'private builder root in source archive payload')
    for name, link in (('/' + 'root/private/source.c', ''), ('source.c', '/' + 'root/private/source.c')):
        archive(payload, name, link=link)
        rejects(lambda: check(source, []), 'private source archive member metadata')
    archive(payload, 'app/source.c', b'/rooted/public\n/home/runner-other/public')
    check(source, [])
    with patch.object(Path, 'home', return_value=Path('/home/runner')):
        check(source, [])
        archive(payload, 'app/source.c', b'/home/' + b'runner/private')
        rejects(lambda: check(source, []), 'private builder root in source archive payload')
    payload.unlink()
    (source / 'verification.json').write_bytes(b'/' + b'root/private')
    rejects(lambda: check(source, []), 'private builder root in source file')
    (source / 'verification.json').unlink()
    # Exact private HEAD export, using production's reviewed exclusions.
    subprocess.run(['git', 'archive', '--format=tar', '-o', str(source / 'navi5.tar'), 'HEAD',
                    '--', '.', ':!docs/media', ':!app/assets/brand', ':!scripts/__pycache__'], cwd=ROOT, check=True)
    check(source, [])
    with tarfile.open(source / 'navi5.tar') as tar:
        names = set(tar.getnames())
        for name in ('LICENSE', 'scripts/assemble-source.py', 'scripts/build-release-ffmpeg.sh',
                     'docs/SOURCE.md', 'docs/RECIPIENT_RELINK.md', 'app/scripts/build.sh'):
            assert name in names, name
        assert not any(n.startswith(('docs/media/', 'app/assets/brand/', 'scripts/__pycache__/')) for n in names)
    # Optional exact 77-input cache: no downloads, source bytes unchanged.
    if sys.argv[1:]:
        cache = Path(sys.argv[1])
        pins = json.loads((ROOT / 'scripts/source-inputs.json').read_text())
        for pin in pins:
            shutil.copyfile(cache / pin['file'], source / pin['file'])
        check(source, pins)
        with tarfile.open(source / 'libpsl-0.21.5.tar.gz') as tar:
            assert b'/' + b'root/' in tar.extractfile('libpsl-0.21.5/list/public_suffix_list.dat').read()
print('PASS: pinned upstream root URLs accepted; tampering, private payload/metadata rejected; exact HEAD export preserves sources/licenses/recipes')
