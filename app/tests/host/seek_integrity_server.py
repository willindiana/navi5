# Navi5 — loopback integrity fixtures. SPDX-License-Identifier: GPL-3.0-or-later
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
        if self.path.startswith('/track.') or self.path in ('/late.flac', '/deny.flac'):
            body = (root / ('track.flac' if self.path in ('/late.flac', '/deny.flac') else self.path[1:])).read_bytes()
            if self.path == '/deny.flac' and self.headers.get('Range'):
                self.send_response(403)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            start = 0
            if self.headers.get('Range'):
                start = int(self.headers['Range'][6:-1])
                self.send_response(206)
                self.send_header('Content-Range', f'bytes {start}-{len(body)-1}/{len(body)}')
            else:
                self.send_response(200)
            self.send_header('Content-Length', str(len(body)-start))
            self.end_headers()
            if self.path == '/late.flac' and self.headers.get('Range'):
                body = body[:-100]
                self.close_connection = True
            try:
                self.wfile.write(body[start:])
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        self.send_response(206)
        self.send_header('Content-Range', 'bytes 0-99/101' if self.path == '/wrong-total' else 'bytes 0-99/100')
        if self.path == '/duplicate':
            self.send_header('Content-Range', 'broken')
        length = '1' if self.path == '/short' else '9223372036854775808' if self.path == '/overflow-length' else '100'
        self.send_header('Content-Length', length)
        self.end_headers()
        self.wfile.write(b'x')
        self.close_connection = True


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    subprocess.run([str(root / 'seek-integrity-test'), f'http://127.0.0.1:{server.server_port}'], check=True, timeout=90)
finally:
    server.shutdown()
    server.server_close()
