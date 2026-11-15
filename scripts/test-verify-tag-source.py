#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline regression: a packaged third-party source asset pins its tag commit.

Builds a throwaway repository and asset with the real packager, then checks the
verifier accepts only a tag at the recorded commit whose archive equals the
export validated at build time. No network, builds or consoles.
"""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pack, check = load('package-release'), load('verify-tag-source')
ENV = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_SYSTEM': os.devnull,
       'GIT_AUTHOR_NAME': 'fixture', 'GIT_AUTHOR_EMAIL': 'fixture@example.invalid',
       'GIT_COMMITTER_NAME': 'fixture', 'GIT_COMMITTER_EMAIL': 'fixture@example.invalid'}


def git(repo, *args):
    return subprocess.check_output(['git', *args], cwd=repo, env=ENV, text=True).strip()


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    repo = root / 'repo'
    (repo / 'docs/media').mkdir(parents=True)
    git(repo, 'init', '-q')
    (repo / 'main.cpp').write_text('int main() {}\n')
    (repo / 'docs/media/shot.png').write_bytes(b'art without a grant')
    (repo / '.gitattributes').write_text('docs/media export-ignore\n')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', 'built')
    revision = git(repo, 'rev-parse', 'HEAD')
    # The export the delivery gate validated: same exclusions, by pathspec.
    export = subprocess.check_output(['git', 'archive', '--format=tar', 'HEAD', '--', '.', ':!docs/media'], cwd=repo, env=ENV)

    kit = root / 'kit'
    (kit / 'source').mkdir(parents=True)
    members = {'navi5.tar': export, 'manifest.json': json.dumps({'revision': revision, 'status': 'COMPLETE'}).encode(),
               'fribidi-1.0.15.tar.xz': b'lgpl source', 'relink.tar': b'objects', 'modified-fribidi.a': b'archive',
               'modified-fribidi.c': b'changed', 'verification.json': b'{}'}
    for name, data in members.items():
        (kit / 'source' / name).write_bytes(data)
    (kit / 'source/SHA256SUMS').write_text(''.join(f'{pack.digest(data)}  {name}\n' for name, data in sorted(members.items())))
    original = root / 'original.zip'
    with zipfile.ZipFile(original, 'w') as z:
        for name, data in {'NAVI00001/eboot.bin': b'app', 'NAVI00001/licenses/LICENSE': b'license',
                           'NAVI00001/sce_sys/param.json': json.dumps({'titleId': 'NAVI00001', 'applicationCategoryType': 65536,
                                                                       'contentVersion': '01.000.000'}).encode()}.items():
            info = zipfile.ZipInfo(name)
            info.external_attr = 0o100777 << 16
            z.writestr(info, data)
    (kit / 'validation.json').write_text(json.dumps({'revision': revision,
        'source_status': 'COMPLETE; independently replayed recipient source kit',
        'sha256': {'app/build/app/original.zip': pack.digest(original.read_bytes())}}))
    pack.package(original, kit, root / 'out')
    asset = root / 'out/Navi5-01.000.000-third-party-source.zip'

    def rejects(path, tag, why):
        try:
            check.verify(path, tag, repo)
        except (ValueError, subprocess.CalledProcessError, KeyError):
            return
        raise AssertionError('accepted: ' + why)

    rejects(asset, 'v1.0.0', 'tag that does not exist')
    git(repo, 'tag', 'v1.0.0')
    assert check.verify(asset, 'v1.0.0', repo) == revision
    git(repo, 'tag', '-a', 'v1.0.0-annotated', '-m', 'release')   # Annotated tags peel to the commit.
    assert check.verify(asset, 'v1.0.0-annotated', repo) == revision

    # A tag on any later commit is a different release, even a docs-only one.
    (repo / 'README.md').write_text('later\n')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', 'after build')
    git(repo, 'tag', 'v1.0.1')
    rejects(asset, 'v1.0.1', 'tag on a later commit')
    rejects(asset, 'HEAD', 'moving ref on another commit')
    assert check.verify(asset, 'v1.0.0', repo) == revision

    # Tampered or stale assets: wrong commit in the manifest, an added export, broken sums.
    def rewrite(name, change):
        with zipfile.ZipFile(asset) as z:
            files = {n: z.read(n) for n in z.namelist()}
        change(files)
        path = root / name
        with zipfile.ZipFile(path, 'w') as z:
            for n, data in files.items():
                z.writestr(n, data)
        return path

    def resum(files):
        inner = sorted(n for n in files if n.startswith('source/') and n != 'source/SHA256SUMS')
        files['source/SHA256SUMS'] = ''.join(f'{pack.digest(files[n])}  {n[7:]}\n' for n in inner).encode()
        files['SHA256SUMS'] = ''.join(f'{pack.digest(files[n])}  {n}\n' for n in sorted(files) if n != 'SHA256SUMS').encode()

    def other_commit(files):
        files['source/manifest.json'] = json.dumps({'revision': 'f' * 40, 'status': 'COMPLETE'}).encode()
        resum(files)
    rejects(rewrite('other.zip', other_commit), 'v1.0.0', 'manifest naming another commit')

    def incomplete(files):
        files['source/manifest.json'] = json.dumps({'revision': revision, 'status': 'INCOMPLETE'}).encode()
        resum(files)
    rejects(rewrite('incomplete.zip', incomplete), 'v1.0.0', 'INCOMPLETE manifest')

    def with_export(files):
        files['source/navi5.tar'] = export
        resum(files)
    rejects(rewrite('duplicate.zip', with_export), 'v1.0.0', 'asset carrying a repository export again')

    def unsummed(files):
        files['source/relink.tar'] = b'swapped objects'
    rejects(rewrite('unsummed.zip', unsummed), 'v1.0.0', 'relink capture changed without checksums')

    def extra(files):
        files['source/unlisted.bin'] = b'not covered'
    rejects(rewrite('extra.zip', extra), 'v1.0.0', 'file outside checksum coverage')

    # The archive must still equal what was validated: un-ignoring art changes it.
    git(repo, 'checkout', '-q', 'v1.0.0')
    (repo / '.gitattributes').write_text('')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', 'ship the art')
    git(repo, 'tag', 'v1.0.0-art')
    rejects(asset, 'v1.0.0-art', 'tag whose archive would include excluded art')
print('PASS: tag must be the recorded commit with the validated export; moved tags and tampered assets rejected')
