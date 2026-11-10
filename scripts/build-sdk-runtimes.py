#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""Build pinned SDK LLVM runtimes offline; install only into a fresh staging prefix."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent


def build(cache, sdk, work):
    pins = json.loads((ROOT / 'scripts/source-inputs.json').read_text())
    selected = [p for p in pins if 'llvm_path' in p or p['file'].startswith(
        ('sdk-', 'libcxx-', 'libcxxabi-', 'libunwind-', 'cmake-', 'runtimes-'))]
    for pin in selected:
        path = cache / pin['file']
        with path.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != pin['sha256']:
                raise ValueError('checksum mismatch: ' + pin['file'])
    if work.exists():
        raise ValueError('work directory must not exist; no overwrite or shared sysroot installation')
    work.mkdir(parents=True)
    source = work / 'source'
    source.mkdir()
    for pin in selected:
        path = cache / pin['file']
        if 'llvm_path' in pin:
            target = source / pin['llvm_path']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.read_bytes())
        elif pin['file'].startswith('sdk-'):
            with tarfile.open(path) as archive:
                base = archive.getnames()[0].split('/')[0]
                recipe = archive.extractfile(base + '/libcxx.sh').read().decode()
        else:
            with tarfile.open(path) as archive:
                archive.extractall(source, filter='data')
            component = pin['file'].split('-18.1.8')[0]
            (source / (component + '-18.1.8.src')).rename(source / component)
    # Preserve every SDK runtime flag. Only omit tests/docs, not library features.
    command = recipe[recipe.index('${CMAKE} -S '):recipe.index('${MAKE} -C ')]
    command = command.replace('$TEMPDIR/llvm-project-$LLVM_VER.src/runtimes', '$TEMPDIR/source/runtimes')
    command = command.replace('${PS5_SYSROOT}', '${NAVI5_RUNTIME_PREFIX}')
    command = command.replace('-DCMAKE_BUILD_TYPE=Release',
                              '-DCMAKE_BUILD_TYPE=Release -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF')
    script = ('set -eu\nsource "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"\nunset DESTDIR\n' + command +
              '\n"${CMAKE}" --build "$TEMPDIR/build" --parallel 4\n' +
              '"${CMAKE}" --install "$TEMPDIR/build"\n')
    (work / 'build.sh').write_text(script)
    env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk), TEMPDIR=str(work),
               NAVI5_RUNTIME_PREFIX=str(work / 'install'))
    subprocess.run(['bash', str(work / 'build.sh')], env=env, check=True)
    for name in ['libunwind.a', 'libc++abi.a', 'libc++.a']:
        if not list((work / 'install').rglob(name)):
            raise ValueError('missing installed archive: ' + name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True, help='existing SDK; read only')
    parser.add_argument('--work', type=Path, required=True, help='fresh build/staging directory')
    args = parser.parse_args()
    try:
        build(args.cache.resolve(), args.sdk.resolve(), args.work.resolve())
    except (ValueError, OSError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'ERROR: {error}\n')
