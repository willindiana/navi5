#!/usr/bin/env python3
# Navi5 — check that a release tag is the commit a third-party source asset was built from.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run after tagging and before publishing; read-only, offline.

    python3 -B scripts/verify-tag-source.py Navi5-<version>-third-party-source.zip <tag>

The asset no longer carries Navi5's own source: recipients take the host's
automatic source archive of the release tag. That only works if the tag is
exactly the commit recorded in the asset's source/manifest.json, and if that
archive has the same tracked files as the export validated during the build.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify(asset, tag, repo=ROOT):
    with zipfile.ZipFile(asset) as zipped:
        require(zipped.testzip() is None, 'asset ZIP CRC failure')
        names = set(zipped.namelist())
        require(not any(Path(n).name == 'navi5.tar' for n in names), 'asset still carries a repository export')
        for sums, prefix in (('SHA256SUMS', ''), ('source/SHA256SUMS', 'source/')):
            listed = set()
            for line in zipped.read(sums).decode().splitlines():
                expected, name = line.split('  ', 1)
                require(digest(zipped.read(prefix + name)) == expected, 'checksum mismatch: ' + prefix + name)
                listed.add(prefix + name)
            scope = {n for n in names if n.startswith(prefix) and n != sums} if prefix else names - {sums}
            require(listed == scope, sums + ' does not cover exactly the shipped files')
        manifest = json.loads(zipped.read('source/manifest.json'))
        receipt = json.loads(zipped.read('validation.json'))
        notes = zipped.read('DISTRIBUTION.md').decode()
    revision = manifest.get('revision')
    require(isinstance(revision, str) and re.fullmatch(r'[0-9a-f]{40}', revision), 'manifest revision is not a full commit hash')
    require(manifest.get('status') == 'COMPLETE', 'source manifest is not COMPLETE')
    require(receipt.get('revision') == revision, 'validation receipt names another commit')
    require(revision in notes, 'DISTRIBUTION.md names another commit')
    commit = subprocess.check_output(['git', 'rev-parse', '--verify', '--quiet', tag + '^{commit}'],
                                     cwd=repo, text=True).strip()
    require(commit == revision, f'tag {tag} is commit {commit}, but the asset was built from {revision}')
    # export-ignore (.gitattributes) keeps host archives equal to the validated export.
    export = subprocess.check_output(['git', 'archive', '--format=tar', commit], cwd=repo)
    recorded = re.search(r'SHA-256 ([0-9a-f]{64})\) is not', notes)
    require(recorded is not None, 'DISTRIBUTION.md lacks the validated export digest')
    require(digest(export) == recorded.group(1), 'tag archive content differs from the export validated at build time')
    print(f'PASS: tag {tag} is commit {revision}; tag archive matches the validated source export')
    return revision


if __name__ == '__main__':
    try:
        require(len(sys.argv) == 3, 'usage: verify-tag-source.py ASSET.zip TAG')
        verify(sys.argv[1], sys.argv[2])
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)
