# Navi5 — loopback stream fixtures. SPDX-License-Identifier: GPL-3.0-or-later
import http.server
from pathlib import Path
import subprocess
import sys
import threading
from urllib.parse import parse_qs, urlsplit

root = Path(sys.argv[1])
requests = []
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        url = urlsplit(self.path)
        track = parse_qs(url.query).get('id', [''])[0]
        with lock:
            requests.append((url.path, track))
        status, body = 404, b''
        if url.path == '/rest/stream':
            if track in ('A', 'B'):
                status, body = 200, (root / f'{track}.flac').read_bytes()
            elif track == 'B-output':
                status, body = 200, (root / 'B.flac').read_bytes()
            elif track == 'B-bad':
                status, body = 200, b'not supported audio\n'
            elif track == 'B-denied':
                status, body = 403, b'forbidden\n'
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# Same ephemeral-loopback/subprocess lifecycle as audio_server.py; no LAN listener.
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    for second in ('B', 'B-bad', 'B-denied', 'B-output'):
        with lock:
            requests.clear()
        subprocess.run([str(root / 'playback-audio-test'),
                        f'http://127.0.0.1:{server.server_port}', second],
                       check=True, timeout=25)
        with lock:
            assert sorted(requests) == sorted([('/rest/stream', 'A'), ('/rest/stream', second)]), requests
        print(f'PASS: {second}: exactly one HTTP request per track; no retry/skip', flush=True)
finally:
    server.shutdown()
    server.server_close()
