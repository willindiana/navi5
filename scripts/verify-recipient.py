#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""Verify recipient capture replay and modified FriBidi conversion, offline only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import re
import os

ROOT = Path(__file__).resolve().parents[1]
MARKERS = (b'/Users/', b'Users/', b'/home/', b'home/', b'/private/var/', b'/var/folders/')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def privacy(path):
    data = path.read_bytes()
    if any(marker in data for marker in MARKERS):
        raise ValueError('private path marker in recipient input: ' + path.name)


def verify(capture, modified, work):
    if work.exists() or not work.resolve().is_relative_to((ROOT / 'build').resolve()):
        raise ValueError('fresh work directory under build/ required')
    privacy(capture)
    privacy(modified)
    work.mkdir(parents=True)
    with tarfile.open(capture) as archive:
        seen = set()
        for member in archive:
            path = Path(member.name)
            if (not member.isfile() or path.is_absolute() or '..' in path.parts
                    or str(path) != member.name or member.name in seen):
                raise ValueError('unsafe recipient member')
            seen.add(member.name)
        archive.extractall(work, filter='data')
    relink = work / 'relink'
    version = subprocess.check_output(['ld.lld', '--version'], text=True).strip()
    if (relink / 'version.txt').read_text().strip() != version.removesuffix(' (compatible with GNU linkers)'):
        raise ValueError('linker version differs from capture')
    subprocess.run(['ld.lld', '@response.txt'], cwd=relink, check=True)
    baseline = sha(relink / 'llvm-pie.elf')
    if baseline != sha(ROOT / 'app/build/llvm-pie.elf'):
        raise ValueError('normalized replay differs from built ELF')
    archives = list(relink.rglob('libfribidi.a'))
    if len(archives) != 1:
        raise ValueError('expected one FriBidi archive')
    before = sha(archives[0])
    if before == sha(modified):
        raise ValueError('replacement library is unchanged')
    shutil.copyfile(modified, archives[0])
    subprocess.run(['ld.lld', '@response.txt'], cwd=relink, check=True)
    changed = sha(relink / 'llvm-pie.elf')
    if changed == baseline:
        raise ValueError('modified FriBidi did not change linked ELF')
    build = ROOT / 'app/build'
    sdk = Path(os.environ.get('PS5_PAYLOAD_SDK', ROOT / 'build/release-sdk'))
    tool = build / 'host/ps5-native-tool'
    args = [str(tool), 'link', '--in', str(relink / 'llvm-pie.elf'), '--out', str(work / 'eboot.elf'),
            '--stub-dir', str(sdk / 'target/lib'), '--module-sdk', '0x02000009',
            '--companion-sdk', '0x08050001', '--file-name', 'eboot.elf']
    for stub in sorted((build / 'stubs').glob('*.so')):
        args.extend(['--stub', str(stub)])
    subprocess.run(args, check=True)
    headers = subprocess.check_output(['llvm-readelf', '-lW', str(work / 'eboot.elf')], text=True)
    offsets = re.findall(r'^\s*LOAD\s+(0x[0-9a-fA-F]+).*?\sR\s+0x', headers, re.M)
    if not offsets or int(offsets[0], 16) % 65536:
        raise ValueError('modified read-only segment is not 64 KiB aligned')
    subprocess.run([str(tool), 'self', '--sign', '--in', str(work / 'eboot.elf'),
                    '--out', str(work / 'eboot.bin'), '--magic', '0x1D3D154F'], check=True)
    inspection = subprocess.check_output([str(tool), 'self', '--inspect', '--file', str(work / 'eboot.bin')], text=True)
    if 'integrity: valid' not in inspection or 'container: signed, plaintext' not in inspection:
        raise ValueError('modified SELF validation failed')
    privacy(work / 'eboot.bin')
    dynamic = subprocess.check_output(['llvm-readelf', '-d', str(relink / 'llvm-pie.elf')], text=True)
    if 'libkernel.sprx' not in dynamic or 'libkernel_' in dynamic or 'PosixForWebKit' in dynamic:
        raise ValueError('modified imports invalid')
    record = {'revision': (build / 'source-revision.txt').read_text().strip(),
              'linker': version, 'capture_sha256': sha(capture), 'baseline_elf_sha256': baseline,
              'original_fribidi_sha256': before, 'modified_fribidi_sha256': sha(modified),
              'modified_elf_sha256': changed, 'modified_self_sha256': sha(work / 'eboot.bin'),
              'scope': 'offline replay/conversion only; no console or hardware acceptance'}
    (work / 'verification.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('modified_fribidi', type=Path)
    parser.add_argument('work', type=Path)
    args = parser.parse_args()
    verify(args.capture.resolve(), args.modified_fribidi.resolve(), args.work.resolve())
