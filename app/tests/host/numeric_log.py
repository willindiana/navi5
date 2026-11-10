#!/usr/bin/env python3
# Navi5 — runnable numeric-only UDP envelope validation.
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import sys
from pathlib import Path
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('receiver', Path(__file__).resolve().parents[3] / 'scripts/numeric-log.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
row = b'sock=1/7/2/0/0'
assert m.numeric(b'[123.456] EVO boot: ' + row + b'\n') == row.decode()
for data in (b'boot server=private', row, b'[123.456] EVO boot: ' + row + b' address=private', b'[123.456] EVO boot: ' + row + b'\nprivate', b'\xff'):
    assert m.numeric(data) is None
assert m.numeric(b'[1.000] EVO boot: ok=0 failure=2 curl=7 http=0/0 os=0/0 lookup=0.000045/0 connect=0.000000/0 total=0.000106/0 trace=3/0 first=2/2/-1/22 log=1\n')
assert m.numeric(b'live=0/0/0/0/0/0/0\n') == 'live=0/0/0/0/0/0/0'
assert m.numeric(b'live=1/1/2/4/2048/0/0\n') == 'live=1/1/2/4/2048/0/0'
for data in (b'live=0/5/0/0/0/0/0', b'live=0/1/4/secret/0/0/0', b'prefix live=0/0/0/0/0/0/0', b'live=0/0/0/0/0/0/0\nsecret'):
    assert m.numeric(data) is None
print('PASS: numeric-only UDP envelopes and live rows; boot text and addresses discarded')
