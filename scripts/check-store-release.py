#!/usr/bin/env python3
# Navi5 — read-only deployed release verification; never submit or publish.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check explicit release identity, not mutable latest; pending returns exit 2."""
import argparse
import hashlib
from http.client import HTTPException
import json
from pathlib import Path
import re
import sys
import time
from urllib.request import Request, urlopen

REPO = 'https://github.com/willindiana/navi5'
CATALOG = 'https://prosperostore.net/catalog/v1.json'


def read_json(url):
    with urlopen(Request(url, headers={'User-Agent': 'Navi5-release-check',
                                      'Cache-Control': 'no-cache'}), timeout=30) as response:
        return json.load(response)


def check(catalog, tag, version, sha256):
    if not isinstance(catalog, dict) or not isinstance(catalog.get('apps'), list):
        raise ValueError('unsupported catalog schema')
    if not all(isinstance(row, dict) for row in catalog['apps']):
        raise ValueError('unsupported catalog entry')
    rows = [row for row in catalog['apps'] if row.get('titleid') == 'NAVI00001']
    if len(rows) != 1:
        return 'pending: expected one NAVI00001 listing'
    row = rows[0]
    expected = {'name': 'Navi5', 'source_repo': REPO, 'version': tag,
                'artifact_url': f'{REPO}/releases/download/{tag}/Navi5-{version}.zip',
                'sha256': sha256, 'format': 'zip', 'category': 'music',
                'page': 'https://prosperostore.net/app/NAVI00001/'}
    mismatches = [key for key, value in expected.items() if row.get(key) != value]
    return 'pending: catalog mismatch: ' + ', '.join(mismatches) if mismatches else 'verified'


def poll(fetch, tag, version, sha256, attempts, interval, sleep=time.sleep):
    for attempt in range(attempts):
        try:
            status = check(fetch(), tag, version, sha256)
        except (OSError, HTTPException, ValueError, KeyError, TypeError) as error:
            status = f'pending: catalog unavailable or unsupported schema ({type(error).__name__})'
        print(f'Prospero {attempt + 1}/{attempts}: {status}', flush=True)
        if status == 'verified':
            return 0
        if attempt + 1 < attempts:
            sleep(interval)
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--content-version', required=True)
    parser.add_argument('--sha256', required=True, help='expected install hash from independently approved frozen receipt')
    parser.add_argument('--install', type=Path, help='optional local install ZIP to compare with expected hash; never source ZIP')
    parser.add_argument('--attempts', type=int, default=1)
    parser.add_argument('--interval', type=int, default=60)
    args = parser.parse_args()
    numeric = r'(?:0|[1-9][0-9]*)'
    prerelease = rf'(?:{numeric}|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)'
    if not re.fullmatch(rf'v{numeric}\.{numeric}\.{numeric}(?:-{prerelease}(?:\.{prerelease})*)?', args.tag):
        parser.error('invalid release tag')
    if not re.fullmatch(r'[0-9]{2}\.[0-9]{3}\.[0-9]{3}', args.content_version):
        parser.error('invalid content version')
    if not re.fullmatch(r'[0-9a-f]{64}', args.sha256):
        parser.error('expected lowercase SHA-256 from independently approved receipt')
    if args.install:
        if args.install.name != f'Navi5-{args.content_version}.zip' or not args.install.is_file():
            parser.error('expected frozen install ZIP with matching content version')
        try:
            with args.install.open('rb') as install:
                digest = hashlib.file_digest(install, 'sha256').hexdigest()
        except OSError:
            parser.error('cannot read install ZIP')
        if digest != args.sha256:
            parser.error('install ZIP differs from approved expected SHA-256')
    if not 1 <= args.attempts <= 20 or not 1 <= args.interval <= 300:
        parser.error('attempts must be 1..20; interval must be 1..300 seconds')
    print('RecompStore: pending approval; verification disabled (not a failure)')
    return poll(lambda: read_json(CATALOG), args.tag, args.content_version,
                args.sha256, args.attempts, args.interval)


if __name__ == '__main__':
    sys.exit(main())
