#!/usr/bin/env python3
# Navi5 — offline capture regression; requires LLVM clang and ld.lld on PATH.
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

spec = importlib.util.spec_from_file_location('prepare', Path(__file__).with_name('prepare-relink.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def capture(path, members):
    with tarfile.open(path, 'w') as archive:
        for name, data in members:
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))


with tempfile.TemporaryDirectory(prefix='navi5-relink-') as tmp:
    root = Path(tmp)
    source = root / 'raw.tar'
    output = root / 'safe.tar'
    members = [('relink/response.txt', b'--chroot .\n-o llvm-pie.elf\n"Users/person/build space/a.o"\n'),
               ('relink/version.txt', b'LLD test\n'),
               ('relink/Users/person/build space/a.o', b'object')]
    capture(source, members)
    module.prepare(source, output)
    with tarfile.open(output) as archive:
        assert b'Users/' not in archive.extractfile('relink/response.txt').read()
        assert archive.extractfile('relink/inputs/0000/a.o').read() == b'object'
        assert all(m.uid == m.gid == m.mtime == 0 for m in archive)
    try:
        module.prepare(source, output)
        raise AssertionError('existing output overwritten')
    except FileExistsError:
        pass
    for bad in [members + [members[-1]],
                members[:-1] + [('relink/../escape', b'bad')],
                members[:-1] + [(members[-1][0], b'/Users/private/source.c')],
                [(members[0][0], b'@nested.rsp'), *members[1:]],
                [(members[0][0], b'-lfoo'), *members[1:]],
                [(members[0][0], b'--unknown foo'), *members[1:]]]:
        capture(source, bad)
        target = root / 'refused.tar'
        try:
            module.prepare(source, target)
            raise AssertionError('unsafe capture accepted')
        except ValueError:
            assert not target.exists()
    # End-to-end baseline and ABI-compatible modified static library. Real LLD
    # quoting is exercised under spaces, not merely Python parser round trips.
    work = root / 'build space'
    work.mkdir()
    clang, lld = shutil.which('clang'), shutil.which('ld.lld')
    ar = shutil.which('llvm-ar')
    assert clang and lld and ar, 'LLVM clang, ld.lld and llvm-ar required'
    def run(*args, cwd=work):
        subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (work / 'app.c').write_text('extern int value(void); int _start(void) { return value(); }\n')
    (work / 'lib.c').write_text('int value(void) { return 1; }\n')
    for name in ('app', 'lib'):
        run(clang, '-target', 'x86_64-unknown-freebsd', '-g',
            f'-ffile-prefix-map={root}=/navi5', f'-ffile-prefix-map={root.resolve()}=/navi5',
            '-fdebug-compilation-dir=/navi5/app', '-c', str(work / f'{name}.c'), '-o', f'{name}.o')
        assert str(root).encode() not in (work / f'{name}.o').read_bytes()
    run(ar, 'rcs', 'libvalue.a', 'lib.o')
    run(lld, '--reproduce=raw.tar', '-o', 'llvm-pie.elf', '-L', str(work),
        str(work / 'app.o'), str(work / 'libvalue.a'))
    baseline = (work / 'llvm-pie.elf').read_bytes()
    module.prepare(work / 'raw.tar', work / 'kit.tar', [str(root)])
    dest = root / 'replay space'
    dest.mkdir()
    with tarfile.open(work / 'kit.tar') as archive:
        archive.extractall(dest, filter='data')
    replay = dest / 'relink'
    run(lld, '@response.txt', cwd=replay)
    assert (replay / 'llvm-pie.elf').read_bytes() == baseline
    archive = next((replay / 'inputs').rglob('*.a'))
    (work / 'lib.c').write_text('int value(void) { return 2; }\n')
    run(clang, '-target', 'x86_64-unknown-freebsd', '-c', 'lib.c', '-o', 'lib.o')
    run(ar, 'rcs', str(archive), 'lib.o')
    run(lld, '@response.txt', cwd=replay)
    assert (replay / 'llvm-pie.elf').read_bytes() != baseline
print('capture rejection, prefix-map, unchanged and replacement-library replay passed (host only)')
