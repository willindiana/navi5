#!/usr/bin/env python3
# Navi5
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read LLD map; emit path-free archive/member inventory, not clearance."""
import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import re


def inventory(path):
    raw = path.read_bytes()
    archives = {}
    for library, member in re.findall(r'([^/\\:\s()]+\.a)\(([^)]+)\)', raw.decode()):
        if '/' in member or '\\' in member or PureWindowsPath(member).drive or member in ('.', '..'):
            # Do not echo rejected member: error output must not leak private paths.
            raise ValueError('path-bearing archive member rejected; no inventory emitted')
        archives.setdefault(library, set()).add(member)
    if not archives:
        raise ValueError('no static archive members found in LLD map')
    return {'map_sha256': hashlib.sha256(raw).hexdigest(),
            'scope': 'map-mentioned members; not proof every section survives garbage collection',
            'archives': {name: sorted(members) for name, members in sorted(archives.items())}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('map', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(inventory(args.map), indent=2))
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f'ERROR: {error}\n')
