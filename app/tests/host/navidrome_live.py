#!/usr/bin/env python3
# Navi5 — API key login against real Navidrome servers (docker or CI services).
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs app/tests/host/api_key_live.cpp, built on the app's own transport, against:
  develop  a key-capable Navidrome: issue, use, describe and revoke a key,
           over plain HTTP and again over HTTPS with a disposable CA
  stable   a Navidrome release: stays on password auth unless it offers keys

Servers come from NAVI5_LIVE_DEVELOP_URL / NAVI5_LIVE_STABLE_URL (CI service
containers), or are started here with docker on loopback and removed afterwards.
Only a disposable account is used: it is created on a fresh server, never on an
existing one. Nothing here contacts a console or a personal server.

  python3 app/tests/host/navidrome_live.py              # both servers
  python3 app/tests/host/navidrome_live.py develop      # one of them
Images: NAVI5_LIVE_DEVELOP_IMAGE (deluan/navidrome:develop),
        NAVI5_LIVE_STABLE_IMAGE (deluan/navidrome:latest).
"""
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "app"
USER, PASSWORD = "navi5-ci", "navi5-ci \"disposable\" pass"
IMAGES = {"develop": os.environ.get("NAVI5_LIVE_DEVELOP_IMAGE", "deluan/navidrome:develop"),
          "stable": os.environ.get("NAVI5_LIVE_STABLE_IMAGE", "deluan/navidrome:latest")}


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def post(url, body):
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def wait_ready(url):
    for _ in range(90):
        try:
            with urllib.request.urlopen(url + "/ping", timeout=2) as r:
                if r.status == 200:
                    return
        except OSError:
            pass
        time.sleep(1)
    sys.exit("Navidrome did not become ready")


def account(url):
    """Disposable admin on a fresh server; an existing account must already be ours."""
    status, _ = post(url + "/auth/createAdmin", {"username": USER, "password": PASSWORD})
    if status != 200 and post(url + "/auth/login", {"username": USER, "password": PASSWORD})[0] != 200:
        sys.exit("server already has other accounts: refusing to use a non-disposable Navidrome")


def build(out, ca=None):
    cc, cxx = os.environ.get("CC", "clang"), os.environ.get("CXX", "clang++")
    san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    inc = ["-I" + str(APP / "engine/addons/include")]
    run(cc, *san, "-O1", "-w", "-c", APP / "engine/addons/src/cJSON.c", *inc, "-o", out / "cJSON.o")
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "libavutil", "libcurl"], check=True,
                           capture_output=True, text=True).stdout.split()
    define = [f'-DNAVI5_CA_BUNDLE="{ca}"'] if ca else []
    binary = out / ("api_key_live_tls" if ca else "api_key_live")
    run(cxx, *san, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", *define, *inc,
        "-I" + str(APP / "src/subsonic"), "-I" + str(APP / "src"), "-I" + str(APP / "src/jf"),
        APP / "src/subsonic/subsonic.cpp", APP / "src/subsonic/api_key.cpp",
        APP / "tests/host/api_key_live.cpp", APP / "tests/host/jf_http_curl.cpp", out / "cJSON.o",
        *flags, "-o", binary)
    return binary


def live(binary, mode, url):
    env = dict(os.environ, NAVI5_LIVE_URL=url, NAVI5_LIVE_USER=USER, NAVI5_LIVE_PASSWORD=PASSWORD)
    if subprocess.run([str(binary), mode], env=env).returncode:
        sys.exit(f"live {mode} check failed")


def tls_front(out, upstream):
    """HTTPS on localhost with a one-day CA, forwarding bytes to the HTTP server."""
    key, cert = out / "tls.key", out / "tls.pem"
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost",
        "-addext", "subjectAltName=DNS:localhost", "-keyout", key, "-out", cert,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    host, port = upstream.split("://", 1)[1].rstrip("/").rsplit(":", 1)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    listener = socket.create_server(("127.0.0.1", 0))

    def pump(src, dst):
        try:
            while data := src.recv(65536):
                dst.sendall(data)
        except OSError:
            pass
        finally:
            for s in (src, dst):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def serve():
        while True:
            try:
                raw, _ = listener.accept()
            except OSError:
                return
            try:
                client = context.wrap_socket(raw, server_side=True)
                server = socket.create_connection((host, int(port)), timeout=10)
            except OSError:
                raw.close()
                continue
            server.settimeout(None)
            threading.Thread(target=pump, args=(client, server), daemon=True).start()
            threading.Thread(target=pump, args=(server, client), daemon=True).start()

    threading.Thread(target=serve, daemon=True).start()
    return f"https://localhost:{listener.getsockname()[1]}", cert


def offers_keys(url):
    query = f"u={USER}&p=enc:{PASSWORD.encode().hex()}&v=1.16.1&c=navi5-ci&f=json"
    with urllib.request.urlopen(f"{url}/rest/getOpenSubsonicExtensions?{query}", timeout=10) as r:
        listed = json.load(r)["subsonic-response"].get("openSubsonicExtensions", [])
    return any(e.get("name") == "apiKeyAuthentication" and 1 in e.get("versions", []) for e in listed)


def main():
    wanted = sys.argv[1:] or ["develop", "stable"]
    if any(w not in IMAGES for w in wanted):
        sys.exit(__doc__)
    started = []
    out = Path(tempfile.mkdtemp(prefix="navi5-live-"))
    try:
        urls = {}
        for name in wanted:
            url = os.environ.get(f"NAVI5_LIVE_{name.upper()}_URL", "").rstrip("/")
            if not url:
                if not shutil.which("docker"):
                    sys.exit(f"set NAVI5_LIVE_{name.upper()}_URL or install docker")
                port, container = free_port(), f"navi5-live-{name}-{os.getpid()}"
                run("docker", "run", "-d", "--rm", "--name", container, "-p", f"127.0.0.1:{port}:4533",
                    "-e", "ND_LOGLEVEL=warn", IMAGES[name], stdout=subprocess.DEVNULL)
                started.append(container)
                url = f"http://127.0.0.1:{port}"
            wait_ready(url)
            account(url)
            urls[name] = url
        plain = build(out)
        if "develop" in urls:
            print("develop over HTTP:", flush=True)
            live(plain, "issue", urls["develop"])
            https, ca = tls_front(out, urls["develop"])
            print("develop over HTTPS (disposable CA):", flush=True)
            live(build(out, ca), "issue", https)
        if "stable" in urls:
            # A release that has gained API keys must pass the full check instead.
            mode = "issue" if offers_keys(urls["stable"]) else "fallback"
            print(f"stable ({mode}):", flush=True)
            live(plain, mode, urls["stable"])
        print("PASS: Navidrome live API key checks")
    finally:
        for container in started:
            subprocess.run(["docker", "rm", "-f", container], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
