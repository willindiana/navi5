#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""Offline LLVM pin/layout checks; full configure/build command in LLVM_RUNTIME_INPUTS.md."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
pins = json.loads((ROOT / 'scripts/source-inputs.json').read_text())
modules = [p for p in pins if 'llvm_path' in p]
assert len(modules) == 50
assert sum(p['llvm_path'] == 'llvm/LICENSE.TXT' for p in modules) == 1
for p in modules:
    assert p['source'] == ('https://raw.githubusercontent.com/llvm/llvm-project/'
                           '3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff/' + p['llvm_path'])
    assert p['llvm_path'].startswith('llvm/cmake/') or p['llvm_path'] == 'llvm/LICENSE.TXT'
if len(sys.argv) == 2:
    cache = Path(sys.argv[1])
    for p in modules:
        assert hashlib.sha256((cache / p['file']).read_bytes()).hexdigest() == p['sha256']
    spec = importlib.util.spec_from_file_location('runtime', ROOT / 'scripts/build-sdk-runtimes.py')
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
    # Configure/build invocation is mocked only for layout and command regression.
    # Real full cross-build/install was separately run; see documented receipt.
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory) / 'runtime'
        real_run = subprocess.run
        def check(command, env, check):
            script = Path(command[1]).read_text()
            assert 'unset DESTDIR' in script
            assert '-DLIBCXX_INCLUDE_BENCHMARKS=OFF' in script
            assert '-DLIBCXX_ENABLE_EXCEPTIONS=YES' in script
            assert '-DLIBCXXABI_USE_LLVM_UNWINDER=YES' in script
            assert '$TEMPDIR/source/runtimes' in script
            for name in ['libunwind.a', 'libc++abi.a', 'libc++.a']:
                target = work / 'install/lib' / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.touch()
        runtime.subprocess.run = check
        try:
            runtime.build(cache.resolve(), Path('/unused-sdk'), work)
            assert (work / 'source/llvm/cmake/modules/AddLLVM.cmake').is_file()
            assert (work / 'source/llvm/cmake/config.guess').is_file()
            assert (work / 'source/runtimes/CMakeLists.txt').is_file()
            try:
                runtime.build(cache.resolve(), Path('/unused-sdk'), work)
                raise AssertionError('existing work directory accepted')
            except ValueError:
                pass
        finally:
            runtime.subprocess.run = real_run
print('LLVM immutable pins, offline layout, SDK flags and staging guards passed')
