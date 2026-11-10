#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package two public assets from validated delivery; never publish or rebuild."""
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
    require(json.loads((kit / 'source/manifest.json').read_text())['revision'] == receipt['revision'], 'source revision mismatch')
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
             **{str(p.relative_to(kit)): p.read_bytes() for p in paths}}
    # Root checksums describe archive members, not separate install ZIP.
    files['DISTRIBUTION.md'] = (f'# Distribution identity\n\nBuild revision: {receipt["revision"]}.\n'
        f'Original build archive: {original.name}; {original.stat().st_size} bytes; SHA-256 {digest(original.read_bytes())}.\n'
        f'Distributed folder-only archive: {install.name}; {install.stat().st_size} bytes; SHA-256 {digest(install.read_bytes())}.\n'
        'Every title-folder byte and member metadata preserved. No rebuild during packaging.\n'
        'validation.json preserves original build hashes, not distributed ZIP identity.\n'
        'Extract source/navi5.tar for source and build/relink instructions.\n'
        'source/SHA256SUMS remains unchanged; root SHA256SUMS covers only this source archive.\n'
        'This recipe checks rebuilds; binary identity across LLVM versions is not promised.\n').encode()
    files['SHA256SUMS'] = ''.join(f'{digest(data)}  {name}\n' for name, data in sorted(files.items())).encode()
    source = output / f'Navi5-{param["contentVersion"]}-source.zip'
    with zipfile.ZipFile(source, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, (2026, 11, 10, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            zipped.writestr(info, data, compresslevel=9)
    with zipfile.ZipFile(source) as zipped:
        require(zipped.testzip() is None and set(zipped.namelist()) == set(files), 'source ZIP validation failed')
        for name, data in files.items():
            require(zipped.read(name) == data, 'source ZIP bytes changed')
    require(set(output.iterdir()) == {install, source}, 'expected exactly two assets')
    print(f'Packaging PASS: revision {receipt["revision"]}; exactly two ZIPs; CRCs, bytes and checksum coverage verified', flush=True)
    for path in (install, source):
        print(f'ASSET {path.name}: {path.stat().st_size} bytes; SHA-256 {digest(path.read_bytes())}', flush=True)
    return install


if __name__ == '__main__':
    package(*sys.argv[1:])
