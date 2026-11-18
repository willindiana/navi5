# Navi5 — local-only fixture transport. SPDX-License-Identifier: GPL-3.0-or-later
import http.server
import os
import subprocess
import sys
import threading
import time

os.chdir(sys.argv[1])
assert os.path.getsize('large.flac') > 16 * 1024 * 1024


drop_ranges = []


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        try:
            if self.path == '/stall':
                time.sleep(3)
            elif self.path == '/redirect':
                self.send_response(302)
                self.send_header('Location', '/tone.flac')
                self.end_headers()
            elif self.path in ('/denied', '/unauthorized'):
                self.send_error(403 if self.path == '/denied' else 401)
            elif self.path == '/bad':
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'not music')
            elif self.path == '/unsupported':
                # Existing M4A container, recognized but unsupported PCM codec tag.
                with open('tone.m4a', 'rb') as f:
                    data = f.read().replace(b'mp4a', b'ulaw').replace(b'esds', b'free')
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            elif self.path == '/drop':
                # First request: a full-length 200 whose connection drops halfway,
                # as a proxy does to a long-paused transfer. Ranged requests are
                # answered properly, so the player can resume.
                with open('tone.flac', 'rb') as f:
                    data = f.read()
                wanted = self.headers.get('Range')
                if wanted:
                    start = int(wanted.removeprefix('bytes=').split('-')[0])
                    drop_ranges.append(start)
                    self.send_response(206)
                    self.send_header('Content-Range', f'bytes {start}-{len(data) - 1}/{len(data)}')
                    self.send_header('Content-Length', str(len(data) - start))
                    self.end_headers()
                    self.wfile.write(data[start:])
                else:
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data[:len(data) // 2])
                    self.wfile.flush()
                    self.close_connection = True
                    self.connection.shutdown(2)
            elif self.path == '/drop-ranges':
                body = ','.join(map(str, drop_ranges)).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == '/partial':
                self.send_response(200)
                self.send_header('Content-Length', str(10000000))
                self.end_headers()
                with open('tone.flac', 'rb') as f:
                    self.wfile.write(f.read())
            else:
                super().do_GET()
        except (BrokenPipeError, ConnectionResetError):
            pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    subprocess.run(['./audio-test', f'http://127.0.0.1:{server.server_port}'], check=True)
finally:
    server.shutdown()
    server.server_close()
