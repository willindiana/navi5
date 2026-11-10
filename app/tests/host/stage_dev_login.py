#!/usr/bin/env python3
# Navi5 — offline temporary FTP staging guard check.
# SPDX-License-Identifier: GPL-3.0-or-later
import ftplib
import importlib.util
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
spec = importlib.util.spec_from_file_location('stage', Path(sys.path[0]) / 'stage-dev-login.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class FTP:
    def __init__(self):
        self.files = {m.BASE + '/sce_sys/param.json': json.dumps({'titleId': 'NAVI00001', 'contentId': m.CONTENT_ID}).encode()}
        self.writes = []

    def retrbinary(self, cmd, cb):
        cb(self.files[cmd[5:]])

    def retrlines(self, cmd, cb):
        assert cmd == 'LIST ' + m.BASE
        for path in self.files:
            if path.rsplit('/', 1)[0] == m.BASE:
                cb('-rw------- 1 root root 1 Jan 1 00:00 ' + path.rsplit('/', 1)[1])

    def storbinary(self, cmd, data):
        assert cmd[5:] in m.PATHS
        self.writes.append(cmd[5:])
        self.files[cmd[5:]] = data.read()

    def sendcmd(self, cmd):
        raise AssertionError('staging must not chmod: PS5 app cannot read FTP-owned 600 files')

    def delete(self, path):
        assert path in m.PATHS
        del self.files[path]


ftp = FTP()
logs = []
m.stage(ftp, (b'fixture-secret', b'fixture-config'), logs.append)
assert ftp.writes == list(m.PATHS)
assert not any('fixture-' in row for row in logs)
try:
    m.stage(ftp, (b'x', b'y'), logs.append)
    assert False
except ValueError:
    pass
m.cleanup(ftp)
assert not any(p in ftp.files for p in m.PATHS)
ftp.files[m.BASE + '/sce_sys/param.json'] = b'{"titleId":"PPSA99505"}'
try:
    m.stage(ftp, (b'x', b'y'), logs.append)
    assert False
except ValueError:
    pass
assert len(ftp.writes) == 2
# Host parser must reject final symlink and all hardlinks; ancestors must be trusted.
import os
import tempfile
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'regular'
    path.write_bytes(b'fixture')
    path.chmod(0o600)
    assert m.private(path, 32) == b'fixture'
    alias = Path(directory) / 'alias'
    alias.symlink_to(path)
    try:
        m.private(alias, 32)
        assert False
    except OSError:
        pass
    alias.unlink()
    os.link(path, alias)
    for candidate in (path, alias):
        try:
            m.private(candidate, 32)
            assert False
        except ValueError:
            pass
print('Temporary FTP mock and private parser symlink/hardlink checks passed.')
