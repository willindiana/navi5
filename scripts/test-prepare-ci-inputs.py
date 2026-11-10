#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""Offline fresh-run prep: exact pins, cache reuse, hash rejection, safe extraction."""
import importlib.util
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile

spec = importlib.util.spec_from_file_location('prep', Path(__file__).with_name('prepare-ci-inputs.py'))
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)
assert len(prep.PINS) == 77


def rejects(call):
    try:
        call()
    except (ValueError, subprocess.CalledProcessError):
        return
    raise AssertionError('unsafe input accepted')


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    archive = root / 'source.tar'
    for name, kind, link in [('../escape', tarfile.REGTYPE, ''),
                             ('/absolute', tarfile.REGTYPE, ''),
                             ('fribidi-1.0.15/link', tarfile.SYMTYPE, '../../escape'),
                             ('fribidi-1.0.15/link', tarfile.LNKTYPE, '/escape'),
                             ('fribidi-1.0.15/fifo', tarfile.FIFOTYPE, '')]:
        with tarfile.open(archive, 'w') as target:
            member = tarfile.TarInfo(name)
            member.type = kind
            member.linkname = link
            target.addfile(member)
        rejects(lambda: prep.extract_source(archive, root / 'out'))
    with tarfile.open(archive, 'w') as target:
        member = tarfile.TarInfo('fribidi-1.0.15/lib/fribidi.c')
        member.size = 2
        target.addfile(member, io.BytesIO(b'ok'))
    prep.extract_source(archive, root / 'out')
    assert (root / 'out/fribidi-1.0.15/lib/fribidi.c').read_bytes() == b'ok'
    # Exercise both real extractors; FFmpeg extractor is embedded Python in recipe.
    ffmpeg = (prep.ROOT / 'scripts/build-release-ffmpeg.sh').read_text()
    extractor = ffmpeg.split('python3 - "$SOURCE" "$WORK" <<\'PY\'\n', 1)[1].split('\nPY', 1)[0]
    for prefix in ['fribidi-1.0.15', 'ffmpeg-7.1.1']:
        def extract():
            if prefix.startswith('fribidi'):
                prep.extract_source(archive, root / 'safe')
            else:
                subprocess.run(['python3', '-c', extractor, str(archive), str(root / 'safe')],
                               check=True, capture_output=True)
        for names in [[prefix + '/lib/file', prefix + '/lib/./file'],
                      [prefix + '/lib/file', prefix + '//lib/file'],
                      [prefix + '/lib/file', prefix + '/lib/file'],
                      [prefix + '/lib/', prefix + '/lib'],
                      [prefix + '/../escape']]:
            with tarfile.open(archive, 'w') as target:
                for name in names:
                    member = tarfile.TarInfo(name)
                    if name.endswith('/') or name == prefix + '/lib':
                        member.type = tarfile.DIRTYPE
                    target.addfile(member)
            rejects(extract)
        for kind in [tarfile.SYMTYPE, tarfile.LNKTYPE]:
            with tarfile.open(archive, 'w') as target:
                member = tarfile.TarInfo(prefix + '/link')
                member.type = kind
                member.linkname = '../../escape'
                target.addfile(member)
            rejects(extract)
        with tarfile.open(archive, 'w') as target:
            member = tarfile.TarInfo(prefix + '/lib/')
            member.type = tarfile.DIRTYPE
            target.addfile(member)
            member = tarfile.TarInfo(prefix + '/lib/file')
            member.size = 2
            target.addfile(member, io.BytesIO(b'ok'))
        extract()
        assert (root / 'safe' / prefix / 'lib/file').read_bytes() == b'ok'
    # Existing helper must reuse valid cache without invoking curl.
    cache = root / 'cache'
    cache.mkdir()
    (cache / 'pin').write_bytes(b'pinned')
    helper = str(prep.ROOT / 'scripts/download.sh')
    command = ['bash', '-euo', 'pipefail', '-c',
               'source "$1"; DL="$2"; curl() { return 99; }; fetch https://invalid "$3" pin',
               'test', helper, str(cache), prep.sha(cache / 'pin')]
    subprocess.run(command, check=True)
    (cache / 'pin').write_bytes(b'tampered')
    rejects(lambda: subprocess.run(command, check=True))
    # Downloaded bytes with wrong hash must not replace existing cache.
    command[4] = 'source "$1"; DL="$2"; curl() { echo corrupt > "$DL/pin.part"; }; fetch https://invalid "$3" pin'
    rejects(lambda: subprocess.run(command, check=True))
    assert (cache / 'pin').read_bytes() == b'tampered'
script = (prep.ROOT / 'scripts/ci-release.sh').read_text()
assert script.index('--fetch-only') < script.index('build-release-ffmpeg.sh') < script.index('bash scripts/build.sh --release')
assert 'prepare-ci-inputs.py --offline' in script
assert 'scripts/verify-recipient.py' in script and 'scripts/check-ci-release.py --delivery' in script
print('fresh-run source cache, checksum and extraction regressions passed')
