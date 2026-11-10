#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch reviewed source pins, then build offline replacement regression library."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PINS = json.loads((ROOT / 'scripts/source-inputs.json').read_text())


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fetch_sources(cache):
    cache.mkdir(parents=True, exist_ok=True)
    for pin in PINS:
        if Path(pin['file']).name != pin['file'] or not pin['source'].startswith('https://'):
            raise ValueError('unsafe source pin')
        subprocess.run(['bash', '-euo', 'pipefail', '-c',
                        'source "$1"; DL="$2"; fetch "$3" "$4" "$5"', 'fetch',
                        str(ROOT / 'scripts/download.sh'), str(cache),
                        pin['source'], pin['sha256'], pin['file']], check=True)
    for pin in PINS:
        if (cache / pin['file']).is_symlink() or sha(cache / pin['file']) != pin['sha256']:
            raise ValueError('source checksum mismatch: ' + pin['file'])


def extract_source(archive, work):
    # Reject links and special files too: this source tarball needs neither.
    with tarfile.open(archive) as source:
        seen = set()
        for member in source:
            name = member.name[:-1] if member.isdir() and member.name.endswith('/') else member.name
            path = PurePosixPath(name)
            if (path.is_absolute() or '..' in path.parts or not path.parts
                    or str(path) != name or path.parts[0] != 'fribidi-1.0.15'
                    or not (member.isfile() or member.isdir()) or str(path) in seen):
                raise ValueError('unsafe FriBidi source member: ' + member.name)
            seen.add(str(path))
        source.extractall(work, filter='data')


def build_fribidi(cache, work):
    pin = next(p for p in PINS if p['file'] == 'fribidi-1.0.15.tar.xz')
    archive = cache / pin['file']
    if archive.is_symlink() or sha(archive) != pin['sha256']:
        raise ValueError('FriBidi source checksum mismatch')
    if work.exists() or not work.resolve().is_relative_to((ROOT / 'build').resolve()):
        raise ValueError('fresh work directory under build/ required')
    work.mkdir(parents=True)
    extract_source(archive, work)
    source = work / 'fribidi-1.0.15'
    changed = source / 'lib/fribidi.c'
    original = changed.read_bytes()
    if original.count(b'"Configure options"') != 1:
        raise ValueError('reviewed FriBidi change target missing')
    changed.write_bytes(original.replace(b'"Configure options"', b'"Navi5 local relink check; Configure options"'))
    env = os.environ.copy()
    env.update(CC='prospero-clang', AR='prospero-ar', RANLIB='prospero-ranlib',
               CFLAGS='-O2 -g0', CPPFLAGS='', LDFLAGS='', LC_ALL='C', SOURCE_DATE_EPOCH='0')
    # Neutral configure arguments; no builder roots or debug paths in delivered archive.
    with (work / 'configure.log').open('w') as log:
        subprocess.run(['./configure', '--prefix=/sdk', '--host=x86_64-pc-freebsd',
                        '--enable-static', '--disable-shared'], cwd=source, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    with (work / 'build.log').open('w') as log:
        subprocess.run(['make', '-j4', '-C', 'lib'], cwd=source, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    library = source / 'lib/.libs/libfribidi.a'
    if b'Navi5 local relink check; Configure options' not in library.read_bytes():
        raise ValueError('modified source missing from built library')
    return library, changed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT / 'toolchain/source-cache')
    parser.add_argument('--work', type=Path, default=ROOT / 'build/ci-modified-fribidi')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--fetch-only', action='store_true')
    mode.add_argument('--offline', action='store_true', help='never download; still verify FriBidi pin')
    args = parser.parse_args()
    if not args.offline:
        fetch_sources(args.cache)
    if not args.fetch_only:
        library, source = build_fribidi(args.cache.resolve(), args.work.resolve())
        print('Modified FriBidi built from pinned source; independent link/conversion still required')
        print(library)
        print(source)
