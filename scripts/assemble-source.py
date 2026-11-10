#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline preparation only; incomplete inputs fail closed. See docs/SOURCE.md."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def assemble(cache, output, relink, verification=None, modified_library=None, modified_source=None):
    if output.exists():
        raise ValueError('output already exists; choose a fresh ignored build/ directory')
    if not output.resolve().is_relative_to((ROOT / 'build').resolve()):
        raise ValueError('output must be under ignored repository build/')
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'], cwd=ROOT):
        raise ValueError('tracked/untracked source changes present; commit reviewed source first')
    pins = json.loads((ROOT / 'scripts/source-inputs.json').read_text())
    inputs = []
    for item in pins:
        path = cache / item['file']
        if not path.is_file() or sha(path) != item['sha256']:
            raise ValueError('missing or mismatched input: ' + item['file'])
        inputs.append((path, item['sha256']))
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if relink:
        if (relink.parent / 'source-revision.txt').read_text().strip() != revision:
            raise ValueError('relink build revision does not match HEAD')
        # LLD reproduction tar includes exact objects, archives, stubs and linker scripts.
        with tarfile.open(relink) as archive:
            names = archive.getnames()
            if not any(n.endswith('/response.txt') for n in names):
                raise ValueError('not an LLD reproduction archive')
        if verification:
            import importlib.util
            spec = importlib.util.spec_from_file_location('prepare', ROOT / 'scripts/prepare-relink.py')
            prepare = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(prepare)
            # No raw home-root capture ever enters completed recipient source kit.
            output.mkdir(parents=True)
            prepare.prepare(relink, output / 'relink.tar')
        else:
            inputs.append((relink, sha(relink)))
    output.mkdir(parents=True, exist_ok=bool(verification))
    # Unused inherited art/screenshots have no established redistribution grant.
    # Exclude delivery copies only; retain tracked originals and history unchanged.
    subprocess.run(['git', 'archive', '--format=tar', '-o', str(output / 'navi5.tar'), 'HEAD',
                    '--', '.', ':!docs/media', ':!app/assets/brand', ':!scripts/__pycache__'], cwd=ROOT, check=True)
    for path, _ in inputs:
        shutil.copyfile(path, output / path.name)
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    record = {'revision': revision, 'status': 'INCOMPLETE; PREPARATION ONLY; not complete Corresponding Source',
              'relink': 'captured; matching release identity must be checked against build receipt' if relink else 'MISSING',
              'missing_source_coverage': [
                  'Final clean binary/map/source identity and recipient relink/conversion receipt pending',
                  'Relink member names and object/archive contents contain private build roots; recipient-safe capture not prepared',
                  'Modified-library installation instructions must identify supported loader/version and safe install/update route'],
              'source_export_excludes': ['docs/media', 'app/assets/brand', 'scripts/__pycache__'],
              'inputs': pins}
    if verification:
        if not relink or not modified_library or not modified_source:
            raise ValueError('completion requires capture, verification and modified library/source')
        evidence = json.loads(verification.read_text())
        if evidence['revision'] != revision or evidence['capture_sha256'] != sha(output / 'relink.tar'):
            raise ValueError('recipient verification revision/capture mismatch')
        shutil.copyfile(verification, output / 'verification.json')
        shutil.copyfile(modified_library, output / 'modified-fribidi.a')
        shutil.copyfile(modified_source, output / 'modified-fribidi.c')
        record.update(status='INCOMPLETE', missing_source_coverage=['independent delivery verification pending'],
                      relink='normalized capture; independent replay required by delivery gate')
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    files = sorted(p for p in output.iterdir() if p.is_file())
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.name}\n' for p in files))
    if verification:
        # COMPLETE is accepted only after independent source/privacy/replay checks.
        spec = importlib.util.spec_from_file_location('ci', ROOT / 'scripts/check-ci-release.py')
        ci = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ci)
        try:
            candidate = dict(record, status='COMPLETE', missing_source_coverage=[])
            ci.source_gate(candidate, revision, output)
            record = candidate
            (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
            files = sorted(p for p in output.iterdir() if p.is_file() and p.name != 'SHA256SUMS')
            (output / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.name}\n' for p in files))
        except Exception:
            record.update(status='INCOMPLETE', missing_source_coverage=['independent delivery verification failed'])
            (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
            files = sorted(p for p in output.iterdir() if p.is_file() and p.name != 'SHA256SUMS')
            (output / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.name}\n' for p in files))
            raise
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True, help='flat cache containing pinned archives; no downloads')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/source-delivery')
    parser.add_argument('--relink', type=Path, help='app/build/relink.tar from the matching final build; inspect for private paths first')
    parser.add_argument('--verification', type=Path, help='recipient replay verification.json; enables independently checked completion')
    parser.add_argument('--modified-library', type=Path, help='debug-stripped modified FriBidi test archive')
    parser.add_argument('--modified-source', type=Path, help='matching modified fribidi.c')
    args = parser.parse_args()
    try:
        assemble(args.cache, args.output.resolve(), args.relink, args.verification, args.modified_library, args.modified_source)
    except (ValueError, OSError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'ERROR: {error}\n')
