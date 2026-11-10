#!/usr/bin/env python3
# Navi5 — offline credential transport regression (stdlib + clang++, curl, openssl).
# SPDX-License-Identifier: GPL-3.0-or-later
import http.server
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[3]


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix="navi5-transport-") as tmp:
    tmp = Path(tmp)
    for name in ("trusted", "other"):
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
            "-keyout", str(tmp / f"{name}.key"), "-out", str(tmp / f"{name}.pem"))
    bundle = tmp / "ca.pem"
    shutil.copyfile(tmp / "trusted.pem", bundle)
    driver = tmp / "check.cpp"
    driver.write_text('''#include "jf_http.h"
#include "curl_diag.h"
#include <cassert>
#include <iostream>
int main(int argc, char **argv) {
    auto r = jf::http_request("GET", argv[1], {"Authorization: secret-header"}, "", 3);
    std::cout << r.status << "\\n" << r.error << "\\n" << static_cast<int>(r.failure) << "\\n";
    std::cout << jf::curl_diag.line() << "\\n";
    assert(jf::curl_diag.code >= 0);
    assert(jf::curl_diag.http_info == 0 && jf::curl_diag.os_info == 0);
    assert(jf::curl_diag.lookup_info == 0 && jf::curl_diag.connect_info == 0 && jf::curl_diag.total_info == 0);
    assert(jf::CurlDiag{}.line().find("connect=-1.000000/-1") != std::string::npos);
    if (!r.error.empty() && (!r.body.empty() || !r.cookies.empty())) return 2;
}
''')
    binary = tmp / "check"
    run(os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-fsanitize=address,undefined",
        f'-DNAVI5_CA_BUNDLE="{bundle}"', "-I" + str(ROOT / "app/src/jf"),
        str(driver), str(ROOT / "app/tests/host/jf_http_curl.cpp"), "-lcurl", "-o", str(binary))
    hits = []
    redirect = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            hits.append((self.server.server_port, self.path))
            path = self.path.split("?")[0]
            self.send_response(302 if path in redirect else 200)
            if path in redirect:
                self.send_header("Location", redirect[path])
                self.send_header("Set-Cookie", "secret-cookie")
            self.end_headers()
            self.wfile.write(b"secret-server-text")

    servers = []
    for tls in (True, False, True):
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        if tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(tmp / "trusted.pem", tmp / "trusted.key")
            server.socket = context.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        servers.append(server)
    source, plain, secure = [s.server_port for s in servers]
    redirect.update({
        "/down": f"http://localhost:{plain}/sink?secret-query",
        "/cross": f"https://localhost:{secure}/sink?secret-query",
        "/relative": "/sink?secret-query",
    })

    def check(host, path, expected, failure):
        result = run(str(binary), f"https://{host}:{source}{path}?secret-query&u=secret-user&p=secret-password&t=secret-token")
        assert int(result.stdout.splitlines()[0]) == expected, result.stdout
        assert int(result.stdout.splitlines()[2]) == failure, result.stdout
        assert "secret-" not in result.stdout + result.stderr, result

    try:
        check("localhost", "/ok", 200, 0)
        count = len(hits)
        check("127.0.0.1", "/wrong-host", 0, 6)
        assert len(hits) == count, "hostname failure sent credentials"
        shutil.copyfile(tmp / "other.pem", bundle)
        check("localhost", "/untrusted", 0, 6)
        bundle.unlink()
        check("localhost", "/missing-ca", 0, 5)
        bundle.write_text("not a CA certificate")
        check("localhost", "/invalid-ca", 0, 5)
        assert len(hits) == count, "trust failure sent credentials"
        shutil.copyfile(tmp / "trusted.pem", bundle)
        for path in redirect:
            check("localhost", path, 302, 8)
        assert len(hits) == count + len(redirect), "redirect followed"
        assert not any("/sink" in path for _, path in hits), "credential destination reached"
        print("PASS: trusted TLS, hostname/untrusted/missing/invalid CA refusal; "
              "downgrade/cross-origin/relative redirects refused; sanitized errors")
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()
