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
    REVISION = '0123456789abcdef0123456789abcdef01234567'
    (kit / 'source/manifest.json').write_text(json.dumps({'revision': REVISION}))
    (kit / 'source/navi5.tar').write_bytes(b'source fixture')
    third_party = {'fribidi-1.0.15.tar.xz': b'lgpl source', 'ffmpeg-7.1.1.tar.xz': b'lgpl source 2',
                   'relink.tar': b'objects', 'modified-fribidi.a': b'archive', 'modified-fribidi.c': b'changed source',
                   'verification.json': b'{}'}
    for name, data in third_party.items():
        (kit / 'source' / name).write_bytes(data)
    sums = ''.join(f'{pack.digest(p.read_bytes())}  {p.name}\n' for p in sorted((kit / 'source').iterdir()))
    (kit / 'source/SHA256SUMS').write_text(sums)
    original = root / 'original.zip'
    payload = {'NAVI00001/eboot.bin': b'app', 'NAVI00001/licenses/LICENSE': b'license',
               'NAVI00001/sce_sys/param.json': json.dumps({'titleId': 'NAVI00001', 'applicationCategoryType': 65536, 'contentVersion': '01.000.000'}).encode(),
               'NAVI00001.ffpfsc': b'not public', 'licenses/LICENSE': b'license'}
    with zipfile.ZipFile(original, 'w') as z:
        for name, data in payload.items():
            info = zipfile.ZipInfo(name)
            info.external_attr = 0o100777 << 16
            z.writestr(info, data)
    receipt = {'revision': REVISION, 'source_status': 'COMPLETE; independently replayed recipient source kit',
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
    assert not install.with_name('Navi5-01.000.000-source.zip').exists() # Old name is gone.
    with zipfile.ZipFile(install.with_name('Navi5-01.000.000-third-party-source.zip')) as z:
        names = set(z.namelist())
        # The repository export is not shipped twice: the tag archive is Navi5's source.
        assert 'source/navi5.tar' not in names and not any(n.endswith('navi5.tar') for n in names)
        # LGPL relink and third-party GPL/LGPL source coverage is all still there, byte for byte.
        for name, data in third_party.items():
            assert z.read('source/' + name) == data
        assert z.read('source/manifest.json') == (kit / 'source/manifest.json').read_bytes()
        assert names == {'SHA256SUMS', 'DISTRIBUTION.md', 'validation.json', 'source/SHA256SUMS', 'source/manifest.json',
                         *('source/' + n for n in third_party)}
        # Inner checksums cover exactly the shipped source files and verify.
        listed = {}
        for line in z.read('source/SHA256SUMS').decode().splitlines():
            digest, name = line.split('  ', 1)
            listed[name] = digest
            assert pack.digest(z.read('source/' + name)) == digest
        assert set(listed) == {'manifest.json', *third_party}
        # The asset names the exact commit the tag archive must be, in manifest and notes.
        assert json.loads(z.read('source/manifest.json'))['revision'] == REVISION
        notes = z.read('DISTRIBUTION.md').decode()
        assert REVISION in notes and 'tag' in notes and pack.digest(b'source fixture') in notes
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
    # Dropping the export from the asset must not let a kit without it, or without
    # any relink/third-party input, through: validation covered all of them.
    def without(name, label):
        path = kit / 'source' / name
        data = path.read_bytes()
        path.unlink()
        resum = ''.join(f'{pack.digest(p.read_bytes())}  {p.name}\n' for p in sorted((kit / 'source').iterdir()) if p.name != 'SHA256SUMS')
        (kit / 'source/SHA256SUMS').write_text(resum)
        rejects(label)
        path.write_bytes(data)
        (kit / 'source/SHA256SUMS').write_text(sums)
    for name in ('navi5.tar', 'relink.tar', 'modified-fribidi.a', 'modified-fribidi.c', 'verification.json', 'fribidi-1.0.15.tar.xz'):
        without(name, 'kit-without-' + name)
    # A short or symbolic revision cannot identify the tag archive.
    for bad in ('0123456', 'v1.0.0', REVISION.upper()):
        (kit / 'source/manifest.json').write_text(json.dumps({'revision': bad}))
        receipt['revision'] = bad
        (kit / 'validation.json').write_text(json.dumps(receipt))
        without_sums = ''.join(f'{pack.digest(p.read_bytes())}  {p.name}\n' for p in sorted((kit / 'source').iterdir()) if p.name != 'SHA256SUMS')
        (kit / 'source/SHA256SUMS').write_text(without_sums)
        rejects('bad-revision-' + bad)
    (kit / 'source/manifest.json').write_text(json.dumps({'revision': REVISION}))
    (kit / 'source/SHA256SUMS').write_text(sums)
    receipt['revision'] = REVISION
    (kit / 'validation.json').write_text(json.dumps(receipt))
    receipt['revision'] = 'wrong-head'
    (kit / 'validation.json').write_text(json.dumps(receipt))
    rejects('wrong-head')
    receipt['revision'] = REVISION
    receipt['sha256']['app/build/app/original.zip'] = 'wrong-hash'
    (kit / 'validation.json').write_text(json.dumps(receipt))
    rejects('wrong-original')
    for bad_name, bad_mode, bad_version in (
        ('NAVI00001/../escape', 0o100777, '01.000.000'),
        ('NAVI00001/link', 0o120777, '01.000.000'),
        ('NAVI00001/file', 0o100644, '01.000.000'),
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
print('PASS: two assets, title bytes/modes, third-party source without repository export, tag commit identity, checksums; unsafe inputs rejected')
