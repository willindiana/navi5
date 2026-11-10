#!/usr/bin/env python3
# Navi5 — recipient capture preparation.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Normalize trusted LLD --reproduce captures; never edit binary payloads.

Fails on known private path markers in any payload. This is a bounded check,
not a general secrets detector or a release-completeness attestation.
"""
import argparse
import io
from pathlib import Path, PurePosixPath
import re
import shlex
import tarfile


def prepare(source, output, forbidden=()):
    markers = [b'/Users/', b'Users/', b'/home/', b'home/', b'/private/var/',
               b'/var/folders/', *[s.encode() for s in forbidden]]
    if any(not marker for marker in markers):
        raise ValueError('empty forbidden marker')
    with tarfile.open(source, 'r:') as archive:
        files = {}
        for member in archive:
            path = PurePosixPath(member.name)
            if (not member.isfile() or path.is_absolute() or '..' in path.parts
                    or str(path) != member.name or member.name in files):
                raise ValueError('unsafe, duplicate or non-file capture member')
            files[member.name] = archive.extractfile(member).read()
    roots = [name[:-len('response.txt')] for name in files if name.endswith('/response.txt')]
    if len(roots) != 1:
        raise ValueError('expected one response.txt')
    root = roots[0]
    if any(not name.startswith(root) for name in files):
        raise ValueError('members outside capture root')
    response = files.pop(root + 'response.txt').decode('utf-8')
    version = files.pop(root + 'version.txt').decode('utf-8')
    # Preserve basenames and directory relationships: ELF .deplibs may request
    # -lpthread implicitly. Each original directory gets a unique neutral name.
    directories = {str(PurePosixPath(name[len(root):]).parent) for name in files}
    dirs = {name: f'inputs/{i:04d}' for i, name in enumerate(sorted(directories))}
    mapping = {name[len(root):]: dirs[str(PurePosixPath(name[len(root):]).parent)]
               + '/' + PurePosixPath(name).name for name in files}
    tokens = shlex.split(response, posix=True)
    rewritten = []
    switches = {'-pie', '--eh-frame-hdr', '--allow-multiple-definition',
                '--start-group', '--end-group', '--as-needed'}
    values = {'--chroot': {'.'}, '-m': {'elf_x86_64'},
              '-z': {'max-page-size=0x4000'}, '-mllvm': {'-emulated-tls'},
              '--Map': {'link.map'}, '--entry': {'_start'}, '-o': {'llvm-pie.elf'},
              '--exclude-libs': {'ALL'}, '--error-limit': {'0'}, '--hash-style': {'gnu'}}
    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token in mapping:
            rewritten.append(mapping[token])
        elif token in switches:
            rewritten.append(token)
        elif token in values or token in ('--script', '--version-script', '-L'):
            i += 1
            if i == len(tokens):
                raise ValueError('missing response option value')
            value = tokens[i]
            if token in values:
                if value not in values[token]:
                    raise ValueError('unsupported response option value')
                rewritten.extend([token, value])
            elif token == '-L':
                # Preserve -L order for ELF .deplibs such as libpthread.a.
                if value.rstrip('/') not in dirs:
                    raise ValueError('search directory has no captured inputs')
                rewritten.extend([token, dirs[value.rstrip('/')]])
            else:
                if value not in mapping:
                    raise ValueError('uncaptured script')
                data = files[root + value]
                if re.search(rb'\b(INCLUDE|INPUT|GROUP|SEARCH_DIR|AS_NEEDED)\s*\(?', data):
                    raise ValueError('script may search outside explicit capture inputs')
                rewritten.extend([token, mapping[value]])
        else:
            raise ValueError('unsupported response token; review capture syntax')
        i += 1
    # GNU response quoting, not shell quoting: escape backslashes and double quotes.
    quote = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    payloads = {'relink/response.txt': ('\n'.join(map(quote, rewritten)) + '\n').encode(),
                'relink/version.txt': version.encode()}
    payloads.update({'relink/' + mapping[name[len(root):]]: data for name, data in files.items()})
    for name, data in payloads.items():
        if any(marker in data or marker in name.encode() for marker in markers):
            raise ValueError('private path marker remains; rebuild input, do not byte-replace')
    # Unknown path-bearing options must not silently keep a missing input or escape
    # the extraction root. Known LLD capture options use relative paths only.
    if any(token.startswith('/') or '..' in PurePosixPath(token).parts for token in rewritten):
        raise ValueError('absolute or traversing response token')
    # Exclusive output: preserve raw evidence and reject accidental overwrite.
    with Path(output).open('xb') as stream:
        with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as archive:
            for name, data in payloads.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = 0o644
                archive.addfile(member, io.BytesIO(data))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture')
    parser.add_argument('output')
    parser.add_argument('--forbid', action='append', default=[], help='additional literal private marker')
    args = parser.parse_args()
    try:
        prepare(args.capture, args.output, args.forbid)
    except (ValueError, OSError, KeyError, UnicodeError, tarfile.TarError) as exc:
        parser.exit(1, f'relink preparation refused: {exc}\n')
