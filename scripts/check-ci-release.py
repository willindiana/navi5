#!/usr/bin/env python3
# Navi5 — offline RELEASE checks and fail-closed source delivery gate.
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'app/build'
VERSION = 'v0.8.0-beta.1'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check_zip(archive, expected):
    with zipfile.ZipFile(archive) as zipped:
        files = [entry for entry in zipped.infolist() if not entry.is_dir()]
        require(len(files) == len(expected) and {e.filename for e in files} == set(expected), 'ZIP file set mismatch')
        require(zipped.testzip() is None, 'ZIP CRC failure')
        for entry in files:
            require((entry.external_attr >> 16) & 0o170000 != 0o120000, 'ZIP symlink')
            require((entry.external_attr >> 16) & 0o777 == 0o777, 'ZIP permissions not 0777')
            require(hashlib.sha256(zipped.read(entry)).hexdigest() == sha(expected[entry.filename]), 'ZIP bytes mismatch: ' + entry.filename)


def check_build():
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    require((BUILD / 'source-revision.txt').read_text().strip() == revision, 'build revision does not match HEAD')
    require(not subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'], cwd=ROOT), 'source changes present')
    package = BUILD / 'app/NAVI00001'
    runpy.run_path(str(ROOT / 'scripts/check-smoke-package.py'))['check_package'](package)
    param = json.loads((package / 'sce_sys/param.json').read_text())
    require(param['applicationCategoryType'] == 65536, 'RELEASE must use Media 65536')
    for path in package.rglob('*'):
        require(not path.is_symlink(), 'package symlink')
        require(not re.search(r'(?i)(NAVI5_DEV_BUILD|login\.json|\.env|\.log$|\.pem$)', str(path.relative_to(package))) or path.name == 'ca-bundle.pem', 'private/DEV package file')
    binary = (package / 'eboot.bin').read_bytes()
    for marker in (b'NAVI5_DEV_BUILD', b'NAVI5_DEV_TOOLS', b'JELLY5_REMOTE_LOG', b'/Users/', b'/home/'):
        require(marker not in binary, 'DEV/private path marker: ' + marker.decode())
    inspection = subprocess.check_output([str(BUILD / 'host/ps5-native-tool'), 'self', '--inspect', '--file', str(package / 'eboot.bin')], text=True)
    require('integrity: valid' in inspection and 'container: signed, plaintext' in inspection, 'SELF integrity/profile failure')
    dynamic = subprocess.check_output(['llvm-readelf', '-d', str(BUILD / 'llvm-pie.elf')], text=True)
    needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
    require('libkernel.sprx' in needed, 'libkernel.sprx missing')
    require(not any('libkernel_' in name or 'PosixForWebKit' in name for name in needed), 'forbidden kernel import')
    expected = {str(p.relative_to(BUILD / 'app')): p for p in package.rglob('*') if p.is_file()}
    expected.update({str(p.relative_to(BUILD / 'app')): p for p in (BUILD / 'app/licenses').rglob('*') if p.is_file()})
    for path in (package / 'licenses').iterdir():
        require(sha(path) == sha(BUILD / 'app/licenses' / path.name), 'stale ZIP sidecar license')
    pfs = BUILD / 'app/NAVI00001.ffpfsc'
    expected[pfs.name] = pfs
    archive = BUILD / f'app/Navi5-{param["contentVersion"]}.zip'
    check_zip(archive, expected)
    receipt = {'version': VERSION, 'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
               'mode': 'RELEASE; Media 65536; no DEV', 'imports': needed,
               'self': inspection.splitlines()[1:], 'source_status': 'INCOMPLETE; NOT FOR DISTRIBUTION',
               'compiler': subprocess.check_output(['clang', '--version'], text=True).splitlines()[0],
               'linker': subprocess.check_output(['ld.lld', '--version'], text=True).strip(),
               'sha256': {str(p.relative_to(ROOT)): sha(p) for p in (archive, pfs, package / 'eboot.bin', BUILD / 'llvm-pie.elf', BUILD / 'relink.tar', BUILD / 'link.map')}}
    output = ROOT / 'build/ci-validation.json'
    output.parent.mkdir(exist_ok=True)
    output.write_text(json.dumps(receipt, indent=2) + '\n')
    print('RELEASE package/ZIP/licenses/SELF/import checks passed; source gate still required')


def source_gate(manifest, revision, source=None):
    require(manifest.get('revision') == revision, 'source revision does not match build')
    require(manifest.get('status') == 'COMPLETE' and manifest.get('missing_source_coverage') == [],
            'INCOMPLETE source delivery; mandatory inputs unresolved: ' + '; '.join(manifest.get('missing_source_coverage', ['coverage not established'])))
    require(source is not None, 'source directory and independently checked replay evidence required')
    import importlib.util
    import tempfile
    import tarfile
    source = Path(source)
    pins = json.loads((ROOT / 'scripts/source-inputs.json').read_text())
    require(manifest.get('inputs') == pins, 'source pins differ from reviewed recipe')
    require(len(pins) == 77, 'reviewed source coverage changed')
    for pin in pins:
        require(sha(source / pin['file']) == pin['sha256'], 'source input hash mismatch: ' + pin['file'])
    # Inspect decompressed source payloads, not only compressed outer bytes.
    # Generic /Users/ test fixtures are intentional; actual builder home is not.
    private = [str(Path.home()).encode() + b'/']
    for path in source.iterdir():
        require(path.is_file() and not path.is_symlink(), 'nonregular source delivery input')
        require(not any(marker in path.read_bytes() for marker in private), 'private builder root in source file')
        if tarfile.is_tarfile(path):
            with tarfile.open(path, mode='r|*') as archive:
                for member in archive:
                    require(not any(marker in (member.name + member.linkname).encode() for marker in private),
                            'private source archive member metadata')
                    if member.isfile():
                        data = archive.extractfile(member).read()
                        require(not any(marker in data for marker in private),
                                'private builder root in source archive payload')
    require(manifest.get('source_export_excludes') == ['docs/media', 'app/assets/brand', 'scripts/__pycache__'],
            'unreviewed source exclusions')
    with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='delivery-source-check-') as directory:
        expected = Path(directory) / 'navi5.tar'
        subprocess.run(['git', 'archive', '--format=tar', '-o', str(expected), revision,
                        '--', '.', ':!docs/media', ':!app/assets/brand', ':!scripts/__pycache__'], cwd=ROOT, check=True)
        require(sha(expected) == sha(source / 'navi5.tar'), 'source export differs from built revision')
        # Compare modified-library source against pinned preferred source; permit
        # only documented version-string change used by replacement regression.
        with tarfile.open(source / 'fribidi-1.0.15.tar.xz') as archive:
            original = archive.extractfile('fribidi-1.0.15/lib/fribidi.c').read()
        changed = original.replace(b'"Configure options"', b'"Navi5 local relink check; Configure options"')
        require(changed != original and (source / 'modified-fribidi.c').read_bytes() == changed,
                'modified FriBidi source change not reviewed')
        spec = importlib.util.spec_from_file_location('recipient', ROOT / 'scripts/verify-recipient.py')
        recipient = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(recipient)
        # Replay, modification, conversion, alignment, imports and SELF integrity
        # are recomputed, not trusted manifest booleans or a COMPLETE label.
        recipient.verify(source / 'relink.tar', source / 'modified-fribidi.a', Path(directory) / 'replay')
        actual = json.loads((Path(directory) / 'replay/verification.json').read_text())
        require(actual == json.loads((source / 'verification.json').read_text()), 'recipient evidence differs from replay')
    expected_files = {'navi5.tar', 'manifest.json', 'SHA256SUMS', 'relink.tar',
                      'modified-fribidi.a', 'modified-fribidi.c', 'verification.json', *[p['file'] for p in pins]}
    require({p.name for p in source.iterdir()} == expected_files, 'unexpected source delivery files')
    sums = ''.join(f'{sha(p)}  {p.name}\n' for p in sorted(source.iterdir()) if p.name != 'SHA256SUMS')
    require((source / 'SHA256SUMS').read_text() == sums, 'source SHA256SUMS mismatch')


def delivery():
    source = ROOT / 'build/ci-source'
    receipt = json.loads((ROOT / 'build/ci-validation.json').read_text())
    manifest = json.loads((source / 'manifest.json').read_text())
    require(sha(BUILD / 'relink.tar') == receipt['sha256']['app/build/relink.tar'], 'raw relink identity mismatch')
    # Recipient capture changes only member/response paths; recompute normalization
    # from exact captured inputs before accepting independently replayed kit.
    import importlib.util
    import tempfile
    spec = importlib.util.spec_from_file_location('normalize', ROOT / 'scripts/prepare-relink.py')
    normalize = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(normalize)
    with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='delivery-capture-check-') as directory:
        expected = Path(directory) / 'relink.tar'
        normalize.prepare(BUILD / 'relink.tar', expected)
        require(sha(expected) == sha(source / 'relink.tar'), 'recipient capture differs from raw normalization')
    source_gate(manifest, receipt['revision'], source)
    check_build()
    receipt['source_status'] = 'COMPLETE; independently replayed recipient source kit'
    (ROOT / 'build/ci-validation.json').write_text(json.dumps(receipt, indent=2) + '\n')
    output = ROOT / 'build/ci-release'
    output.mkdir()
    shutil.copytree(source, output / 'source')
    for archive in (BUILD / 'app').glob('Navi5-*.zip'):
        shutil.copyfile(archive, output / f'Navi5-{VERSION}.zip')
    shutil.copyfile(ROOT / 'build/ci-validation.json', output / 'validation.json')
    files = sorted(p for p in output.rglob('*') if p.is_file())
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.relative_to(output)}\n' for p in files))


if __name__ == '__main__':
    try:
        if sys.argv[1:] == ['--delivery']:
            delivery()
        elif not sys.argv[1:]:
            check_build()
        else:
            raise ValueError('usage: check-ci-release.py [--delivery]')
    except (ValueError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        sys.exit(f'ERROR: {error}')
