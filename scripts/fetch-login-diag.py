#!/usr/bin/env python3
# Navi5 — FTP RETR only; remote sandbox backing path must be operator-verified.
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import ftplib
from pathlib import Path
import re
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--host-file', type=Path, required=True)
    p.add_argument('--port', type=int, default=2121)
    p.add_argument('--remote-file', required=True, help='Verified FTP-visible backing path for /download0/navi5/login-diag.txt')
    a = p.parse_args()
    try:
        host = a.host_file.read_text().strip()
        if (not host or any(c.isspace() for c in host) or not a.remote_file.startswith('/') or
                any(ord(c) < 32 or ord(c) == 127 for c in a.remote_file)):
            raise ValueError()
        data = bytearray()
        def receive(chunk):
            if len(data) + len(chunk) > 4096:
                raise ValueError()
            data.extend(chunk)
        with ftplib.FTP() as ftp:
            ftp.connect(host, a.port, timeout=10)
            ftp.login()
            ftp.retrbinary('RETR ' + a.remote_file, receive, blocksize=512)
        text = data.decode('ascii')
        if not re.fullmatch(r'ok=[01] failure=\d+ curl=-?\d+ http=-?\d+/-?\d+ os=-?\d+/-?\d+ lookup=-?\d+\.\d+/-?\d+ connect=-?\d+\.\d+/-?\d+ total=-?\d+\.\d+/-?\d+(?: trace=\d+/\d+ first=-?\d+/-?\d+/-?\d+/-?\d+)?(?: log=-?[01])?\n(?:sock=\d+/-?\d+/-?\d+/-?\d+/-?\d+\n){0,16}', text):
            raise ValueError()
        sys.stdout.write(text)
        return 0
    except (OSError, ValueError, EOFError, ftplib.Error):
        print('diagnostic=unavailable mapping=unverified')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
