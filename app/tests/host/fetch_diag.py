#!/usr/bin/env python3
# Navi5 — FTP read-only contract and output boundary.
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('fetch_diag', root / 'scripts/fetch-login-diag.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
line = b'ok=0 failure=4 curl=7 http=0/0 os=61/0 lookup=0.000001/0 connect=0.000000/0 total=0.000002/0\n'

class FTP:
    payload = line
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def connect(self, *args, **kwargs): pass
    def login(self): pass
    def retrbinary(self, cmd, receive, **kwargs):
        assert cmd == 'RETR /verified/login-diag.txt'
        receive(self.payload)

with tempfile.TemporaryDirectory() as d:
    host = Path(d) / 'host'; host.write_text('localhost')
    with patch.object(m.ftplib, 'FTP', FTP), patch.object(sys, 'argv', ['fetch', '--host-file', str(host), '--remote-file', '/verified/login-diag.txt']):
        for payload, status in [(line, 0), (b'secret-server-text', 1), (line.rstrip(b'\n') + b' trace=1/0 first=2/2/-1/22\nsock=0/2/2/-1/22\n', 0), (b'x' * 4097, 1)]:
            FTP.payload = payload
            out = io.StringIO()
            with contextlib.redirect_stdout(out): assert m.main() == status
            assert 'secret' not in out.getvalue()
    for remote in ['relative/login-diag.txt', '/bad\r\nDELE file', '/bad\x00name']:
        with patch.object(m.ftplib, 'FTP') as ftp, patch.object(sys, 'argv', ['fetch', '--host-file', str(host), '--remote-file', remote]):
            with contextlib.redirect_stdout(io.StringIO()): assert m.main() == 1
            ftp.assert_not_called()
print('PASS: RETR only, bounded data, fixed failure output, absolute control-free path')
