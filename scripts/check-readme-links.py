#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit tracked README links. Offline by default; --remote checks public docs only."""
import argparse
import concurrent.futures
import datetime
import html
import json
import posixpath
import re
import subprocess
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# ponytail: small Markdown subset, not a full renderer; review ambiguous syntax manually.
ALLOWED = {'github.com', 'codeload.github.com', 'opensubsonic.netlify.app',
           'www.navidrome.org', 'www.ijg.org', 'www.iso.org', 'www.itu.int',
           'www.w3.org', 'mirrors.ctan.org', 'mirrors.rit.edu', 'latex.us', 'www.faqs.org'}


def visible(text, inline=True):
    text = re.sub(r'(?ms)^\s*(`{3,}|~{3,}).*?^\s*\1[^\n]*',
                  lambda m: '\n' * m[0].count('\n'), text)
    text = re.sub(r'<!--.*?-->', lambda m: '\n' * m[0].count('\n'), text, flags=re.S)
    return re.sub(r'(`+)([^`\n]*?)\1', lambda m: ' ' * len(m[0]), text) if inline else text


def links(text):
    clean = visible(text)
    refs = {re.sub(r'\s+', ' ', m[1]).lower(): m[2]
            for m in re.finditer(r'^\s{0,3}\[([^\]]+)\]:\s*<?([^\s>]+)>?', clean, re.M)}
    found = []
    patterns = [r'!?\[[^\]\n]*\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+["\'][^\n]*?["\'])?\s*\)',
                r'\b(?:href|src)\s*=\s*["\']([^"\']+)["\']',
                r'(?<![\w])https?://[^\s<>"\']+']
    occupied = []
    for pattern in patterns:
        for m in re.finditer(pattern, clean, re.I):
            if any(a <= m.start() < b for a, b in occupied):
                continue
            url = m[1].strip('<>') if m.lastindex else m[0].rstrip('.,;)')
            found.append((clean.count('\n', 0, m.start()) + 1, html.unescape(url)))
            occupied.append(m.span())
    for m in re.finditer(r'!?\[([^\]\n]+)\](?:\[([^\]\n]*)\])?', clean):
        if any(a <= m.start() < b for a, b in occupied):
            continue
        key = re.sub(r'\s+', ' ', m[2] or m[1]).lower()
        if key in refs:
            found.append((clean.count('\n', 0, m.start()) + 1, refs[key]))
    return sorted(set(found))


def anchors(text):
    ids = set(re.findall(r'\b(?:id|name)=["\']([^"\']+)', text))
    lines = visible(text, inline=False).splitlines()
    for i, line in enumerate(lines):
        m = re.match(r'^ {0,3}#{1,6}\s+(.*?)\s*#*$', line)
        title = m[1] if m else (lines[i - 1] if i and re.fullmatch(r'\s*(?:=+|-+)\s*', line) else None)
        if title is None:
            continue
        title = re.sub(r'<[^>]*>', '', html.unescape(title)).lower()
        title = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', title)
        slug = ''.join(c for c in title if c in '-_ ' or not unicodedata.category(c).startswith(('P', 'S', 'C'))).replace(' ', '-')
        base, n = slug, 0
        while slug in ids:
            n += 1
            slug = f'{base}-{n}'
        ids.add(slug)
    return ids


def local(file, url, tracked):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme or parts.netloc:
        return 'unverified scheme'
    target = posixpath.normpath(posixpath.join(posixpath.dirname(file), urllib.parse.unquote(parts.path))) if parts.path else file
    if target.startswith('../') or target.startswith('/'):
        return 'outside repository'
    matches = [p for p in tracked if p == target or p.startswith(target.rstrip('/') + '/')]
    if not matches:
        return 'case mismatch' if any(p.casefold() == target.casefold() for p in tracked) else 'missing local target'
    if parts.fragment:
        path = ROOT / target
        if path.is_dir():
            path /= 'README.md'
        if path.suffix.lower() != '.md':
            return 'unverified non-Markdown anchor'
        if urllib.parse.unquote(parts.fragment) not in anchors(path.read_text()):
            return 'missing anchor'
    return 'ok local'


class PublicRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        p = urllib.parse.urlsplit(newurl)
        if p.scheme not in ('http', 'https') or p.hostname not in ALLOWED or p.port not in (None, 80, 443):
            raise ValueError('redirect outside reviewed documentation hosts: ' + newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def remote(url):
    p = urllib.parse.urlsplit(url)
    if p.hostname not in ALLOWED or p.port not in (None, 80, 443) or p.username or p.password:
        return {'status': 'unverified outside reviewed public documentation hosts'}
    opener = urllib.request.build_opener(PublicRedirect())
    attempts = []
    for method in ('HEAD', 'GET'):
        try:
            req = urllib.request.Request(url, method=method, headers={'User-Agent': 'Navi5-README-link-audit/1.0'})
            with opener.open(req, timeout=18) as response:
                attempts.append({'method': method, 'code': response.status, 'final': response.url})
                if p.fragment:
                    return {'status': 'ok HTTP; remote anchor unverified', 'attempts': attempts}
                return {'status': 'ok HTTP', 'attempts': attempts}
        except urllib.error.HTTPError as e:
            attempts.append({'method': method, 'code': e.code, 'final': e.url})
            e.close()
        except (OSError, ValueError) as e:
            attempts.append({'method': method, 'error': str(e)})
    code = attempts[-1].get('code')
    private = p.hostname == 'github.com' and code == 404
    return {'status': 'unverified GitHub 404 (private/deleted indistinguishable)' if private else
            'dead HTTP' if code in (404, 410) else 'unverified HTTP', 'attempts': attempts}


def self_check():
    assert links('[x](a.md#one) <img src="icon.png">\n[x][r]\n[r]: https://github.com/x/y\n```\nhttps://localhost/test\n```') == [(1, 'a.md#one'), (1, 'icon.png'), (2, 'https://github.com/x/y'), (3, 'https://github.com/x/y')]
    assert anchors('# One\n# One\n## Hello, World!\n') == {'one', 'one-1', 'hello-world'}
    assert local('README.md', 'readme.md', {'README.md'}) == 'case mismatch'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--remote', action='store_true')
    args = parser.parse_args()
    self_check()
    tracked = set(subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')) - {''}
    files = sorted(p for p in tracked if 'readme' in Path(p).name.lower())
    rows = [{'file': f, 'line': n, 'url': u} for f in files for n, u in links((ROOT / f).read_text())]
    urls = sorted({r['url'] for r in rows if r['url'].startswith(('http://', 'https://'))})
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = dict(zip(urls, pool.map(remote, urls))) if args.remote else {}
    for row in rows:
        u = row['url']
        row.update(results.get(u, {'status': 'unverified remote; use --remote'}) if u.startswith(('http://', 'https://')) else {'status': local(row['file'], u, tracked)})
    print(json.dumps({'checked_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'files': files,
                      'remote_unique': len(urls), 'links': rows}, indent=2))


if __name__ == '__main__':
    main()
