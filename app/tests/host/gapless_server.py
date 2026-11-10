# Navi5 — loopback gapless fixtures. SPDX-License-Identifier: GPL-3.0-or-later
import http.server
from pathlib import Path
import subprocess
import sys
import threading

root = Path(sys.argv[1])


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == '/stall':
            self.send_response(200)
            self.send_header('Content-Length', '100')
            self.end_headers()
            threading.Event().wait(3)
            return
        status = 200
        body = b'not audio' if self.path == '/bad' else b''
        if self.path == '/denied':
            status = 403
        elif self.path == '/partial':
            body = (root / 'B.flac').read_bytes()[:-100]
        elif (root / self.path[1:]).is_file():
            body = (root / self.path[1:]).read_bytes()
        start = 0
        if self.headers.get('Range') and status == 200:
            start = int(self.headers['Range'][6:-1])
            status = 206
        self.send_response(status)
        if status == 206:
            self.send_header('Content-Range', f'bytes {start}-{len(body)-1}/{len(body)}')
        self.send_header('Content-Length', str(len(body) - start + (100 if self.path == '/partial' else 0)))
        self.end_headers()
        try:
            self.wfile.write(body[start:])
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    subprocess.run([str(root / 'gapless-test'), f'http://127.0.0.1:{server.server_port}'], check=True, timeout=90)
finally:
    server.shutdown()
    server.server_close()
