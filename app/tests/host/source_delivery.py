#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline source pins/notices/font identity check; optional cache verification."""
import hashlib
import os
import tempfile
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[3]
pins = json.loads((ROOT / 'scripts/source-inputs.json').read_text())
assert len(pins) == 77 and len({p['file'] for p in pins}) == 77
for item in pins:
    assert re.fullmatch('[0-9a-f]{64}', item['sha256'])
    assert '/' not in item['file'] and item['source'].startswith('https://')
for item in json.loads((ROOT / 'docs/FONT_METADATA.json').read_text()):
    assert hashlib.sha256((ROOT / item['file']).read_bytes()).hexdigest() == item['sha256']
if len(sys.argv) == 2:
    cache = Path(sys.argv[1])
    for item in pins:
        assert hashlib.sha256((cache / item['file']).read_bytes()).hexdigest() == item['sha256'], item['file']
    # Offline source-only generation: no pip, installed packages or tracked output writes.
    with tempfile.TemporaryDirectory() as directory:
        scratch = Path(directory)
        for filename, notices in [
            ('babel-2.18.0.tar.gz', [('LICENSE', 'app/licenses/Babel-LICENSE.txt'),
                                   ('babel/locale-data/LICENSE.unicode', 'app/src/app/CLDR-LICENSE.txt')]),
            ('pycountry-24.6.1.tar.gz', [('LICENSE.txt', 'app/licenses/pycountry-LICENSE.txt')])]:
            with tarfile.open(cache / filename) as archive:
                base = archive.getnames()[0].split('/')[0]
                for member, target in notices:
                    assert archive.extractfile(base + '/' + member).read() == (ROOT / target).read_bytes()
                archive.extractall(scratch, filter='data')
        output = scratch / 'i18n_cldr.cpp'
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                   PYTHONPATH=os.pathsep.join([str(scratch / 'babel-2.18.0'),
                                               str(scratch / 'pycountry-24.6.1/src')]))
        subprocess.run([sys.executable, '-S', str(ROOT / 'app/scripts/i18n_cldr.py'), str(output)],
                       env=env, check=True, stdout=subprocess.DEVNULL)
        assert output.read_bytes() == (ROOT / 'app/src/app/i18n_cldr.cpp').read_bytes(), 'CLDR regeneration differs'
    # New recipe pins must agree with the already-pinned PacBrew source, not
    # merely a checksum recorded after download. libjpeg recipe explicitly skips.
    with tarfile.open(next(cache.glob('pacbrew-*.tar.gz'))) as archive:
        base = archive.getnames()[0]
        for item in pins:
            if 'recipe' in item:
                recipe = archive.extractfile(base + '/' + item['recipe']).read().decode()
                assert item['sha256'] in recipe or ('libjpeg-turbo/' in item['recipe'] and "sha256sums=('SKIP')" in recipe)
    for filename, member, notice_file in [
        ('openlibm-0.8.6.tar.gz', 'openlibm-0.8.6/LICENSE.md', 'openlibm-LICENSE.md'),
        ('libjpeg-turbo-3.0.2.tar.gz', 'libjpeg-turbo-3.0.2/README.ijg', 'libjpeg-turbo-README.ijg.txt')]:
        with tarfile.open(cache / filename) as archive:
            assert archive.extractfile(member).read() == (ROOT / 'app/licenses' / notice_file).read_bytes()
    with tarfile.open(cache / 'musl-1.2.5.tar.gz') as archive:
        musl_fnmatch = archive.extractfile('musl-1.2.5/src/regex/fnmatch.c').read()
        assert archive.extractfile('musl-1.2.5/COPYRIGHT').read() == (ROOT / 'app/licenses/musl-COPYRIGHT.txt').read_bytes()
    sdk = next(cache.glob('sdk-*.tar.gz'))
    notice = (ROOT / 'app/licenses/sdk-libc-NOTICES.txt').read_text()
    with tarfile.open(sdk) as archive:
        base = archive.getnames()[0]
        sdk_fnmatch = archive.extractfile(base + '/libc/fnmatch.c').read()
        assert sdk_fnmatch == musl_fnmatch.replace(b'#include "locale_impl.h"', b'//#include "locale_impl.h"')
        headers = json.loads((ROOT / 'docs/SDK_INCLUDED_HEADERS.json').read_text())['headers']
        assert len(headers) == 99
        header_notices = (ROOT / 'app/licenses/sdk-header-NOTICES.txt').read_text()
        for header in headers:
            assert header['sdk_source_matches']
            raw = archive.extractfile(base + '/include/' + header['sdk_source_matches'][0]).read()
            assert hashlib.sha256(raw).hexdigest() == header['sha256']
            match = re.match(r'\s*((?:(?:/\*.*?\*/)|(?://[^\n]*\n)|(?:\s))+)', raw.decode(), re.S)
            if match and match[1].strip():
                assert match[1].strip().replace('\r\n', '\n') in header_notices
        count = 0
        for member in archive.getmembers():
            if not member.isfile() or '/libc/' not in member.name:
                continue
            path = Path(member.name)
            if path.suffix not in ('.c', '.h', '.S', '.inc'):
                continue
            raw = archive.extractfile(member).read()
            match = re.match(r'\s*(/\*.*?\*/|(?://[^\n]*\n)+)', raw.decode(), re.S)
            if match:
                assert hashlib.sha256(raw).hexdigest() in notice, path
                assert match.group(1) in notice, path
                count += 1
        assert count == 79, count
# Archive inventory must retain member identity without disclosing build roots.
import importlib.util
spec = importlib.util.spec_from_file_location('inventory', ROOT / 'scripts/link-source-inventory.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'link.map'
    path.write_text('  123 /private/build/libfoo.a(member.o):(.text)\n 456 /private/build/libfoo.a(member.o):(.data)\n')
    record = module.inventory(path)
    assert record['archives'] == {'libfoo.a': ['member.o']}
    assert '/private' not in json.dumps(record)
    for library in ['/private/build/libfoo.a', r'C:\private\build\libfoo.a']:
        path.write_text(f'{library}(symbol::name.o):(.text)\n')
        assert module.inventory(path)['archives'] == {'libfoo.a': ['symbol::name.o']}
        for member in ['/private/build/member.o', 'relative/member.o', './member.o', '../member.o',
                       r'private\member.o', r'\member.o', r'C:\private\member.o', 'C:member.o', '.', '..']:
            path.write_text(f'{library}({member}):(.text)\n')
            try:
                module.inventory(path)
                raise AssertionError('path-bearing member accepted')
            except ValueError as error:
                assert str(error) == 'path-bearing archive member rejected; no inventory emitted'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/link-source-inventory.py'), str(path)],
                                    capture_output=True)
            assert result.returncode == 1 and not result.stdout
            assert result.stderr == b'ERROR: path-bearing archive member rejected; no inventory emitted\n'
    path.write_text('not a map')
    try:
        module.inventory(path)
        raise AssertionError('empty inventory accepted')
    except ValueError:
        pass
# Source export omits unused uncleared assets/bytecode, not required build inputs.
with tempfile.TemporaryDirectory() as directory:
    exported = Path(directory) / 'source.tar'
    subprocess.run(['git', 'archive', '--format=tar', '-o', str(exported), 'HEAD', '--', '.',
                    ':!docs/media', ':!app/assets/brand', ':!scripts/__pycache__'], cwd=ROOT, check=True)
    with tarfile.open(exported) as archive:
        names = archive.getnames()
        for excluded in ['docs/media', 'app/assets/brand', 'scripts/__pycache__']:
            assert not any(n == excluded or n.startswith(excluded + '/') for n in names)
        for required in ['app/assets/fonts/Roboto-Regular.ttf', 'app/assets/branding/icon.svg',
                         'app/scripts/build.sh', 'toolkit/tools/native-app/ps5-pie.ld', 'LICENSE']:
            assert required in names
fonts = ROOT / 'app/assets/fonts'
assert 'Copyright 2011 Google Inc. All Rights Reserved.' in (fonts / 'Roboto-NOTICE.txt').read_text()
assert 'Copyright 2013 Google Inc. All Rights Reserved.' in (fonts / 'NotoNaskhArabicUI-NOTICE.txt').read_text()
# Refuse outputs outside ignored build/, before even inspecting cache/source state.
result = subprocess.run([sys.executable, str(ROOT / 'scripts/assemble-source.py'),
                         '--cache', '/nonexistent', '--output', str(ROOT / 'not-ignored')], capture_output=True)
assert result.returncode == 1 and b'output must be under ignored' in result.stderr
print('source pins, verbatim SDK comments, font hashes and output guard passed')
