#!/usr/bin/env python3
# Navi5 — explicitly approved, loose runtime FTP input; never package secrets.
# SPDX-License-Identifier: GPL-3.0-or-later
"""FTP is plaintext. Close Navi5 through PS button before staging or cleanup.
Only NAVI00001 is supported. No app launch or connection is triggered.
"""
import argparse
import ftplib
import io
import json
import os
from pathlib import Path
import stat
from urllib.parse import urlsplit
import deploy

BASE = '/data/homebrew/NAVI00001'
PATHS = (BASE + '/.navi5-dev-password.tmp', BASE + '/.navi5-dev-login.tmp')
SUPPORT = Path.home() / 'Library/Application Support/Navi5'
CONTENT_ID = 'UP9000-NAVI00001_00-NAVI500000000000'


def private(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        st = os.fstat(f.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o77 or st.st_nlink != 1:
            raise ValueError('unsafe private input')
        data = f.read(limit + 1)
    if not 0 < len(data) <= limit or b'\0' in data:
        raise ValueError('invalid private input')
    return data


def fields(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate field')
        result[key] = value
    return result


def inputs(config):
    obj = json.loads(private(config, 8192), object_pairs_hook=fields)
    if set(obj) != {'server', 'username', 'password_file'} or not all(isinstance(v, str) and v for v in obj.values()):
        raise ValueError('invalid config')
    s, u, p = obj['server'], obj['username'], obj['password_file']
    url = urlsplit(s)
    if len(s) > 2048 or len(u) > 256 or not Path(p).is_absolute() or len(p) > 4096 or url.scheme not in ('http', 'https') or not url.hostname or url.username is not None or url.password is not None or url.query or url.fragment:
        raise ValueError('invalid config')
    secret = private(p, 1024).decode('utf-8').removesuffix('\n').removesuffix('\r')
    if not secret or any(ord(c) < 32 or ord(c) == 127 for c in s + u + p + secret):
        raise ValueError('invalid input')
    obj['password_file'] = '/app0/.navi5-dev-password.tmp'
    return secret.encode(), json.dumps(obj, ensure_ascii=False).encode()


def guard(ftp):
    obj = json.loads(deploy.read_remote(ftp, BASE + '/sce_sys/param.json', 65536))
    if not isinstance(obj, dict) or obj.get('titleId') != 'NAVI00001' or obj.get('contentId') != CONTENT_ID:
        raise ValueError('title guard failed')


def stage(ftp, payloads, report=print):
    guard(ftp)
    names = {name for name, _ in deploy.listdir(ftp, BASE)}
    if any(path.rsplit('/', 1)[1] in names for path in PATHS):
        raise ValueError('staged files already exist; cleanup first')
    attempted = []
    try:
        for path, data in zip(PATHS, payloads):
            attempted.append(path)  # include partial STOR on failure
            ftp.storbinary('STOR ' + path, io.BytesIO(data))
            # ponytail: no chmod 600. On PS5 the app uid is not the FTP owner, so
            # 600 made the files unreadable (hardware: open EACCES). Plaintext
            # staging accepted by owner; app deletes after import, cleanup verifies.
    finally:
        for path in attempted:
            report('Cleanup path: ' + path)


def cleanup(ftp):
    guard(ftp)
    names = {name for name, _ in deploy.listdir(ftp, BASE)}
    for path in PATHS:
        if path.rsplit('/', 1)[1] in names:
            ftp.delete(path)
    names = {name for name, _ in deploy.listdir(ftp, BASE)}
    if any(path.rsplit('/', 1)[1] in names for path in PATHS):
        raise ValueError('cleanup not verified')
    print('Temporary login files absent; cleanup verified.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--confirm-closed', action='store_true', required=True)
    parser.add_argument('--cleanup', action='store_true')
    parser.add_argument('--config', type=Path, default=SUPPORT / 'dev-login.json')
    parser.add_argument('--host-file', type=Path, default=SUPPORT / 'console-host')
    parser.add_argument('--port', type=int, default=2121)
    args = parser.parse_args()
    try:
        if not 1 <= args.port <= 65535:
            raise ValueError('invalid port')
        host = args.host_file.read_text().strip()
        if not host or any(c.isspace() or ord(c) < 32 for c in host):
            raise ValueError('invalid host')
        payloads = None if args.cleanup else inputs(args.config)
        print('WARNING: FTP transfers plaintext credentials. Dedicated temporary files only.')
        with deploy.connect(host, args.port) as ftp:
            if args.cleanup:
                cleanup(ftp)
            else:
                stage(ftp, payloads)
    except (OSError, ValueError, ftplib.Error, UnicodeError):
        print('Temporary login operation failed; inspect fixed cleanup paths. No retry.')
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
