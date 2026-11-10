#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline regression: forged completeness, mismatched HEAD and ZIP tampering fail."""
import importlib.util
from pathlib import Path
import tempfile
import zipfile
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('ci', Path(__file__).with_name('check-ci-release.py'))
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)


def rejects(call):
    try:
        call()
    except ValueError:
        return
    raise AssertionError('unsafe input accepted')


for manifest in (
    {'revision': 'head', 'status': 'INCOMPLETE', 'missing_source_coverage': ['LLVM runtime']},
    {'revision': 'other', 'status': 'COMPLETE', 'missing_source_coverage': []},
    {'revision': 'head', 'status': 'COMPLETE', 'missing_source_coverage': []},
):
    rejects(lambda: ci.source_gate(manifest, 'head'))

# A COMPLETE label plus source path cannot bypass reviewed source pins.
rejects(lambda: ci.source_gate({'revision': 'head', 'status': 'COMPLETE',
                               'missing_source_coverage': [], 'inputs': []}, 'head', Path('/nonexistent')))

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    file = root / 'LICENSE'
    file.write_text('license bytes')
    archive = root / 'package.zip'
    with zipfile.ZipFile(archive, 'w') as zipped:
        entry = zipfile.ZipInfo('LICENSE')
        entry.external_attr = 0o100777 << 16
        zipped.writestr(entry, file.read_bytes())
    ci.check_zip(archive, {'LICENSE': file})
    file.write_text('stale license')
    rejects(lambda: ci.check_zip(archive, {'LICENSE': file}))
    rejects(lambda: ci.check_zip(archive, {}))
build_script = (ci.ROOT / 'app/scripts/build.sh').read_text()
assert 'if (( ! RELEASE )) && [[ -n "${JELLY5_REMOTE_LOG:-}" ]]' in build_script
# Consume all readelf output: early awk exit makes LLVM fail under pipefail.
import subprocess
awk = '$1=="LOAD" && $7=="R" && $8 ~ /^0x/ && !found {print $2; found=1}'
assert awk in build_script
producer = "print('LOAD 0x10000 0 0 0 0 R 0x10000'); print('x' * 1000000)"
subprocess.run(['bash', '-o', 'pipefail', '-c',
                'python3 -c "$1" | awk "$2" > "$3"', 'test', producer, awk, '/dev/null'], check=True)
assert 'source-revision.txt' in (ci.ROOT / 'scripts/ci-release.sh').read_text()
assert 'source-revision.txt' in (ci.ROOT / 'scripts/assemble-source.py').read_text()
# Exercise real dynamic imports in a child process: this test's sys flag does
# not propagate. Keep source identity strict for tracked AND untracked edits.
import os
import shutil
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'scripts').mkdir()
    (root / 'app/build').mkdir(parents=True)
    for name in ('assemble-source.py', 'check-ci-release.py', 'prepare-relink.py', 'verify-recipient.py'):
        shutil.copyfile(ci.ROOT / 'scripts' / name, root / 'scripts' / name)
    (root / '.gitignore').write_text('app/build/\n')
    subprocess.run(['git', 'init', '-q', str(root)], check=True)
    subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(root), '-c', 'user.name=Fixture', '-c',
                    'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture'], check=True)
    revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True)
    (root / 'app/build/source-revision.txt').write_text(revision)
    probe = '''import importlib.util, runpy, subprocess
from pathlib import Path
runpy.run_path('scripts/assemble-source.py')
for name in ('prepare-relink', 'check-ci-release', 'verify-recipient'):
    spec = importlib.util.spec_from_file_location(name, Path('scripts') / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if name == 'check-ci-release':
        ci = module
assert not list(Path('scripts').glob('__pycache__/*'))
assert not subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'])
def package_reached(_):
    raise RuntimeError('past source guard')
ci.runpy.run_path = package_reached
try:
    ci.check_build()
except RuntimeError as error:
    assert str(error) == 'past source guard'
else:
    raise AssertionError('package check not reached')
for path in (Path('scripts/check-ci-release.py'), Path('scripts/new-source.py')):
    original = path.read_bytes() if path.exists() else None
    path.write_bytes((original or b'') + b'\\n# genuine source edit\\n')
    try:
        ci.check_build()
    except ValueError as error:
        assert str(error) == 'source changes present'
    else:
        raise AssertionError('dirty source accepted')
    if original is None:
        path.unlink()
    else:
        path.write_bytes(original)
assert not subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'])
'''
    # Use actual release bootstrap, not a duplicate environment setting.
    bootstrap = (ci.ROOT / 'scripts/ci-release.sh').read_text().split('ROOT=', 1)[0]
    env = dict(os.environ)
    env.pop('PYTHONDONTWRITEBYTECODE', None)
    subprocess.run(['bash', '-c', bootstrap + '\nexec "$1" -c "$2"',
                    'test', sys.executable, probe], cwd=root, env=env, check=True)
print('CI gate, bytecode isolation, dirty-source rejection, release logging, revision and ZIP regressions passed')
