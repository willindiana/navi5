# Navi5 — loopback range fixtures. SPDX-License-Identifier: GPL-3.0-or-later
import http.server
import os
import subprocess
import sys
import threading
import time

os.chdir(sys.argv[1])
assert os.path.getsize('track.m4a') > 3_000_000
with open('track.m4a', 'rb') as f:
    data = f.read()
assert data.index(b'moov') > data.index(b'mdat')
ranges = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        try:
            name = 'track.m4a' if self.path.endswith('.m4a') else 'track.flac'
            with open(name, 'rb') as f:
                data = f.read()
            start = 0
            request = self.headers.get('Range')
            if request and self.path.startswith('/stall'):
                time.sleep(2)
            if request and not self.path.startswith('/ignore'):
                start = int(request.removeprefix('bytes=').removesuffix('-'))
                ranges.append(start)
                self.send_response(206)
                reported = start + 1 if self.path.startswith('/wrong') else start
                self.send_header('Content-Range', f'bytes {reported}-{len(data)-1}/{len(data)}')
            else:
                self.send_response(200)
            self.send_header('Content-Length', str(len(data) - start))
            self.end_headers()
            self.wfile.write(data[start:])
        except (BrokenPipeError, ConnectionResetError):
            pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    subprocess.run(['./seek-test', f'http://127.0.0.1:{server.server_port}'], check=True)
    assert any(x > 1_000_000 for x in ranges), 'trailing moov never read by range'
    print('PASS: verified actual range offsets, including trailing moov')
finally:
    server.shutdown()
    server.server_close()
