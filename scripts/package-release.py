#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package two public assets from validated delivery; never publish or rebuild.

Navi5-<version>.zip is the install folder. Navi5-<version>-third-party-source.zip
holds what the tag's own source archive cannot: pinned third-party source
tarballs, the recipient relink capture, the modified FriBidi, and the manifest
and verification receipts. Navi5's own source is the host's automatic source
archive of the release tag, which must point at the commit manifest.json records
(scripts/verify-tag-source.py checks that before publication).
"""
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def package(original, kit, output):
    original, kit, output = map(Path, (original, kit, output))
    receipt = json.loads((kit / 'validation.json').read_text())
    require(receipt['source_status'] == 'COMPLETE; independently replayed recipient source kit', 'incomplete delivery')
    require(receipt['sha256']['app/build/app/' + original.name] == digest(original.read_bytes()), 'original build archive mismatch')
    paths = sorted((kit / 'source').iterdir())
    require(all(p.is_file() and not p.is_symlink() for p in paths), 'nonregular source input')
    sums = ''.join(f'{digest(p.read_bytes())}  {p.name}\n' for p in paths if p.name != 'SHA256SUMS')
    require((kit / 'source/SHA256SUMS').read_text() == sums, 'source checksum coverage mismatch')
    revision = receipt['revision']
    require(json.loads((kit / 'source/manifest.json').read_text())['revision'] == revision, 'source revision mismatch')
    # The tag archive stands in for the repository export, so the commit must be exact.
    require(isinstance(revision, str) and re.fullmatch(r'[0-9a-f]{40}', revision), 'revision is not a full commit hash')
    # The validated kit must still contain the export it was checked against...
    require((kit / 'source/navi5.tar').is_file(), 'validated repository export missing from kit')
    export_digest = digest((kit / 'source/navi5.tar').read_bytes())
    # ...and everything LGPL relinking and GPL third-party source coverage need.
    names = {p.name for p in paths}
    for needed in ('manifest.json', 'verification.json', 'relink.tar', 'modified-fribidi.a', 'modified-fribidi.c', 'SHA256SUMS'):
        require(needed in names, 'source kit lacks ' + needed)
    require(any(n.startswith('fribidi-') and '.tar' in n for n in names), 'source kit lacks third-party source tarballs')
    shipped = [p for p in paths if p.name not in ('navi5.tar', 'SHA256SUMS')]
    with zipfile.ZipFile(original) as archive:
        require(archive.testzip() is None, 'original ZIP CRC failure')
        require(len(archive.namelist()) == len(set(archive.namelist())), 'duplicate ZIP member')
        param = json.loads(archive.read('NAVI00001/sce_sys/param.json'))
        require(param['titleId'] == 'NAVI00001' and param['applicationCategoryType'] == 65536, 'not RELEASE Media title')
        require(isinstance(param['contentVersion'], str) and re.fullmatch(r'[0-9]{2}\.[0-9]{3}\.[0-9]{3}', param['contentVersion']), 'invalid content version')
        members = [m for m in archive.infolist() if m.filename.startswith('NAVI00001/')]
        for m in members:
            require('..' not in m.filename.split('/') and '\\' not in m.filename and not m.flag_bits & 1, 'unsafe title member')
            require((m.external_attr >> 16) & 0o170000 in (0o040000, 0o100000), 'nonregular title member')
            require((m.external_attr >> 16) & 0o777 == 0o777, 'title permissions not 0777')
        require(any(m.filename == 'NAVI00001/eboot.bin' and not m.is_dir() for m in members), 'missing eboot.bin')
        require(any(m.filename.startswith('NAVI00001/licenses/') and not m.is_dir() for m in members), 'missing title licenses')
        output.mkdir(parents=True, exist_ok=False)
        install = output / f'Navi5-{param["contentVersion"]}.zip'
        with zipfile.ZipFile(install, 'w') as zipped:
            for member in members:
                zipped.writestr(member, archive.read(member))
        with zipfile.ZipFile(install) as zipped:
            require(zipped.testzip() is None and zipped.namelist() == [m.filename for m in members], 'install ZIP validation failed')
            for member in members:
                require(zipped.read(member.filename) == archive.read(member), 'title bytes changed')
    files = {'validation.json': (kit / 'validation.json').read_bytes(),
             **{str(p.relative_to(kit)): p.read_bytes() for p in shipped}}
    # Same format as the kit's list, minus the repository export that is not shipped.
    files['source/SHA256SUMS'] = ''.join(f'{digest(p.read_bytes())}  {p.name}\n' for p in shipped).encode()
    # Root checksums describe archive members, not separate install ZIP.
    files['DISTRIBUTION.md'] = (f'# Distribution identity\n\nBuild revision: {receipt["revision"]}.\n'
        f'Original build archive: {original.name}; {original.stat().st_size} bytes; SHA-256 {digest(original.read_bytes())}.\n'
        f'Distributed folder-only archive: {install.name}; {install.stat().st_size} bytes; SHA-256 {digest(install.read_bytes())}.\n'
        'Every title-folder byte and member metadata preserved. No rebuild during packaging.\n'
        'validation.json preserves original build hashes, not distributed ZIP identity.\n'
        f'Navi5 source: the automatic source archive of the release tag at commit {revision}.\n'
        'Download that tag archive first; it has the build, relink and install instructions.\n'
        'source/manifest.json records the same commit. A tag at any other commit is not this release.\n'
        f'The export checked during validation (git archive of that commit, SHA-256 {export_digest}) is not\n'
        'shipped here; a host-generated tag archive has different container bytes but the same tracked files,\n'
        'plus docs/media and app/assets/brand, which manifest.json lists as excluded from that export.\n'
        'This archive adds third-party source tarballs, relink.tar, modified FriBidi and receipts.\n'
        'source/SHA256SUMS covers the shipped source files; root SHA256SUMS covers this archive.\n'
        'This recipe checks rebuilds; binary identity across LLVM versions is not promised.\n').encode()
    files['SHA256SUMS'] = ''.join(f'{digest(data)}  {name}\n' for name, data in sorted(files.items())).encode()
    source = output / f'Navi5-{param["contentVersion"]}-third-party-source.zip'
    with zipfile.ZipFile(source, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, (2026, 11, 10, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            zipped.writestr(info, data, compresslevel=9)
    with zipfile.ZipFile(source) as zipped:
        require(zipped.testzip() is None and set(zipped.namelist()) == set(files), 'source ZIP validation failed')
        require('source/navi5.tar' not in zipped.namelist(), 'repository export must not be shipped twice')
        for name, data in files.items():
            require(zipped.read(name) == data, 'source ZIP bytes changed')
    require(set(output.iterdir()) == {install, source}, 'expected exactly two assets')
    print(f'Packaging PASS: revision {receipt["revision"]}; exactly two ZIPs; CRCs, bytes and checksum coverage verified', flush=True)
    print(f'TAG SOURCE: release tag must be commit {revision}; check with scripts/verify-tag-source.py', flush=True)
    for path in (install, source):
        print(f'ASSET {path.name}: {path.stat().st_size} bytes; SHA-256 {digest(path.read_bytes())}', flush=True)
    return install


if __name__ == '__main__':
    package(*sys.argv[1:])
