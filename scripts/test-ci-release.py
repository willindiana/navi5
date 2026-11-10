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
print('CI gate, release logging guard, revision capture and ZIP regressions passed')
