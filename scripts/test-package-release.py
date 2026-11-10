#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline packaging regression; no builds, network or consoles."""
import importlib.util
import json
from pathlib import Path
import tempfile
import zipfile

spec = importlib.util.spec_from_file_location('pack', Path(__file__).with_name('package-release.py'))
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    kit = root / 'kit'
    (kit / 'source').mkdir(parents=True)
    (kit / 'source/manifest.json').write_text(json.dumps({'revision': 'built-revision'}))
    (kit / 'source/navi5.tar').write_bytes(b'source fixture')
    sums = ''.join(f'{pack.digest(p.read_bytes())}  {p.name}\n' for p in sorted((kit / 'source').iterdir()))
    (kit / 'source/SHA256SUMS').write_text(sums)
    original = root / 'original.zip'
    payload = {'NAVI00001/eboot.bin': b'app', 'NAVI00001/licenses/LICENSE': b'license',
               'NAVI00001/sce_sys/param.json': json.dumps({'titleId': 'NAVI00001', 'applicationCategoryType': 65536, 'contentVersion': '00.008.000'}).encode(),
               'NAVI00001.ffpfsc': b'not public', 'licenses/LICENSE': b'license'}
    with zipfile.ZipFile(original, 'w') as z:
        for name, data in payload.items():
            info = zipfile.ZipInfo(name)
            info.external_attr = 0o100777 << 16
            z.writestr(info, data)
    receipt = {'revision': 'built-revision', 'source_status': 'COMPLETE; independently replayed recipient source kit',
               'sha256': {'app/build/app/original.zip': pack.digest(original.read_bytes())}}
    (kit / 'validation.json').write_text(json.dumps(receipt))
    install = pack.package(original, kit, root / 'out')
    assert len(list(install.parent.iterdir())) == 2
    with zipfile.ZipFile(install) as z:
        assert set(z.namelist()) == {n for n in payload if n.startswith('NAVI00001/')}
        assert z.testzip() is None
        for m in z.infolist():
            assert z.read(m) == payload[m.filename]
            assert (m.external_attr >> 16) & 0o777 == 0o777
    with zipfile.ZipFile(install.with_name('Navi5-00.008.000-source.zip')) as z:
        assert z.read('source/SHA256SUMS') == (kit / 'source/SHA256SUMS').read_bytes()
        assert z.read('validation.json') == (kit / 'validation.json').read_bytes()
        assert pack.digest(install.read_bytes()).encode() in z.read('DISTRIBUTION.md')
        for line in z.read('SHA256SUMS').decode().splitlines():
            digest, name = line.split('  ', 1)
            assert pack.digest(z.read(name)) == digest
        assert install.name not in z.namelist()
    def rejects(name):
        try:
            pack.package(original, kit, root / name)
        except ValueError:
            return
        raise AssertionError('unsafe input accepted: ' + name)

    (kit / 'source/extra').write_bytes(b'unhashed')
    rejects('unhashed-source')
    (kit / 'source/extra').unlink()
    receipt['revision'] = 'wrong-head'
    (kit / 'validation.json').write_text(json.dumps(receipt))
    rejects('wrong-head')
    receipt['revision'] = 'built-revision'
    receipt['sha256']['app/build/app/original.zip'] = 'wrong-hash'
    (kit / 'validation.json').write_text(json.dumps(receipt))
    rejects('wrong-original')
    for bad_name, bad_mode, bad_version in (
        ('NAVI00001/../escape', 0o100777, '00.008.000'),
        ('NAVI00001/link', 0o120777, '00.008.000'),
        ('NAVI00001/file', 0o100644, '00.008.000'),
        ('NAVI00001/file', 0o100777, '../../escape'),
    ):
        with zipfile.ZipFile(original, 'w') as z:
            for name, data in {**payload, bad_name: b'bad'}.items():
                if name.endswith('param.json'):
                    param = json.loads(data)
                    param['contentVersion'] = bad_version
                    data = json.dumps(param).encode()
                info = zipfile.ZipInfo(name)
                info.external_attr = (bad_mode if name == bad_name else 0o100777) << 16
                z.writestr(info, data)
        receipt['sha256']['app/build/app/original.zip'] = pack.digest(original.read_bytes())
        (kit / 'validation.json').write_text(json.dumps(receipt))
        rejects('unsafe-member')
print('PASS: two assets, title bytes/modes, source/build identity, checksums; unsafe inputs rejected')
