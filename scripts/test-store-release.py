#!/usr/bin/env python3
# Navi5 — offline deployed release verification regressions.
# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import importlib.util
import contextlib
import hashlib
from http.client import IncompleteRead
import io
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch
from urllib.error import URLError

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('store', Path(__file__).with_name('check-store-release.py'))
store = importlib.util.module_from_spec(spec)
spec.loader.exec_module(store)
tag, version, digest = 'v0.8.0-beta.1', '00.008.000', 'a' * 64
row = dict(titleid='NAVI00001', name='Navi5', source_repo=store.REPO,
           version=tag, artifact_url=f'{store.REPO}/releases/download/{tag}/Navi5-{version}.zip',
           sha256=digest, format='zip', category='music', page='https://prosperostore.net/app/NAVI00001/')
catalog = {'apps': [row]}
assert store.check(catalog, tag, version, digest) == 'verified'
for field in row:
    bad = copy.deepcopy(catalog)
    bad['apps'][0][field] = 'wrong'
    assert store.check(bad, tag, version, digest).startswith('pending:')
for rows in ([], [row, row]):
    assert store.check({'apps': rows}, tag, version, digest).startswith('pending:')
bad = copy.deepcopy(catalog)
bad['apps'][0]['artifact_url'] = row['artifact_url'].replace('.zip', '-source.zip')
assert store.check(bad, tag, version, digest).startswith('pending:')
# Stale cache eventually advances; no sleep after final attempt or success.
sleeps = []
responses = iter([{'apps': []}, catalog])
assert store.poll(lambda: next(responses), tag, version, digest, 3, 2, sleeps.append) == 0
assert sleeps == [2]
sleeps.clear()
assert store.poll(lambda: {}, tag, version, digest, 2, 2, sleeps.append) == 2
assert sleeps == [2]
def unavailable():
    raise URLError('offline')
assert store.poll(unavailable, tag, version, digest, 1, 2, sleeps.append) == 2
assert sleeps == [2]
# Failures during response.read(), not only connection setup, remain pending.
for error in (ConnectionResetError('reset'), IncompleteRead(b'{', 10)):
    sleeps.clear()
    with patch.object(store, 'urlopen') as request:
        response = request.return_value.__enter__.return_value
        response.read.side_effect = error
        assert store.poll(lambda: store.read_json(store.CATALOG), tag, version, digest,
                          3, 2, sleeps.append) == 2
        assert response.read.call_count == 3
        assert request.call_count == 3
    assert sleeps == [2, 2]
# Future release identity is explicit, never inferred from the fetched catalog.
future = copy.deepcopy(catalog)
future_tag, future_version = 'v1.2.3', '01.002.003'
future['apps'][0].update(version=future_tag,
    artifact_url=f'{store.REPO}/releases/download/{future_tag}/Navi5-{future_version}.zip')
assert store.check(future, future_tag, future_version, digest) == 'verified'
assert store.check(future, tag, version, digest).startswith('pending:')
with tempfile.TemporaryDirectory() as directory:
    install = Path(directory) / f'Navi5-{future_version}.zip'
    install.write_bytes(b'frozen fixture')
    expected = hashlib.sha256(install.read_bytes()).hexdigest()
    argv = ['check-store-release.py', '--tag', future_tag, '--content-version', future_version,
            '--sha256', expected, '--install', str(install)]
    with patch.object(sys, 'argv', argv), patch.object(store, 'poll', return_value=0) as poll:
        assert store.main() == 0
        assert poll.call_args.args[1:4] == (future_tag, future_version, expected)
    invalid_args = [argv[:-2] + ['--install', str(install.with_name('source.zip'))],
                    argv[:6] + ['b' * 64] + argv[7:],
                    argv[:6] + ['invalid'] + argv[7:], argv[:5]]
    for invalid_tag in ('v1.2.3-.', 'v1.2.3-beta..1', 'v1.2.3-01', 'v01.2.3',
                        'v1.02.3', 'v1.2.03', 'v1.2.3-', 'v1.2.3-beta.01', 'v1.2.3+build'):
        invalid_args.append(argv[:2] + [invalid_tag] + argv[3:])
    for invalid_version in ('1.002.003', '01.02.003', '01.002.03', '01.002.0034',
                            '01.002.003\n', '01.002.-03', 'aa.002.003'):
        invalid_args.append(argv[:4] + [invalid_version] + argv[5:])
    for invalid in invalid_args:
        with patch.object(sys, 'argv', invalid), patch.object(store, 'read_json') as fetch:
            with contextlib.redirect_stderr(io.StringIO()):
                try:
                    store.main()
                    raise AssertionError('invalid input accepted')
                except SystemExit as error:
                    assert error.code == 2
            fetch.assert_not_called()
    # Workflow needs no local binary; trusted CLI expectation still mandatory.
    for valid_tag in ('v0.0.0', future_tag, tag, 'v1.2.3-0', 'v1.2.3-01a.0.x-y'):
        with patch.object(sys, 'argv', argv[:2] + [valid_tag] + argv[3:-2]), \
                patch.object(store, 'poll', return_value=0):
            assert store.main() == 0
print('Store identity, future versions, trusted hash, CLI rejection and bounded polling passed')
