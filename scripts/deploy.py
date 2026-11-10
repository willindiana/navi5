#!/usr/bin/env python3
# Jelly5 — upload the built app to the PS5 over FTP (ftpsrv / etaHEN).
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Private Navi5 smoke install/update only; no launch, retry or removal.
    scripts/deploy.py --check
    scripts/deploy.py --confirm-closed --confirm-title-unused
    scripts/deploy.py --update --confirm-closed
Update requires remote /data/homebrew/NAVI00001 param.json with matching
titleId/contentId; files are promoted in place via temp names, then read back.
Confirm app closed through PS button and NAVI00001 unused in title database,
all internal/external storage and ShadowMount scan roots before installation.
FTP checks cannot prove lifecycle or inspect every title registration.
Existing titles/staging and unreadable homebrew metadata refuse installation.
"""
import errno
import ftplib
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def env():
    out = {}
    p = os.path.join(ROOT, ".env.local")
    if os.path.exists(p):
        for line in open(p):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                out[k] = v
    return out


def connect(host, port):
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login()
    ftp.set_pasv(True)
    return ftp


def exists(ftp, path):
    # Listing parent distinguishes absence from ambiguous CWD 550 replies.
    parent, name = path.rsplit("/", 1)
    return any(entry == name for entry, _ in listdir(ftp, parent or "/"))


def listdir(ftp, path):
    """(name, is_dir) for each entry; ftpsrv has LIST but not NLST/MLSD."""
    lines = []
    ftp.retrlines(f"LIST {path}", lines.append)
    out = []
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) < 9:
            sys.exit("unrecognized remote listing; nothing sent")
        if parts[8] in (".", ".."):
            continue
        out.append((parts[8], line.startswith("d")))
    return out


LAST = ("eboot.bin", "sce_sys/param.json")


def local_files(local, remote, mkd=None):
    """(local, remote) pairs; eboot.bin then sce_sys/param.json last."""
    files = []
    for dp, dns, fns in os.walk(local):
        dns.sort()
        rel = os.path.relpath(dp, local)
        rdir = remote if rel == "." else f"{remote}/{rel}"
        if mkd:
            try:
                mkd(rdir)
            except ftplib.error_perm:
                pass
        for fn in sorted(fns):
            files.append((os.path.join(dp, fn), f"{rdir}/{fn}"))
    def order(f):
        rel = os.path.relpath(f[0], local).replace(os.sep, "/")
        return LAST.index(rel) + 1 if rel in LAST else 0
    return sorted(files, key=order)


def read_remote(ftp, path, limit=None):
    data = bytearray()
    def collect(chunk):
        data.extend(chunk)
        if limit and len(data) > limit:
            sys.exit(f"remote {path} too large; stopped")
    ftp.retrbinary(f"RETR {path}", collect)
    return bytes(data)


def upload_tree(ftp, local, remote):
    files = local_files(local, remote, ftp.mkd)
    total = sum(os.path.getsize(f[0]) for f in files)
    sent = 0
    t0 = time.time()
    for lp, rp in files:
        with open(lp, "rb") as fh:
            ftp.storbinary(f"STOR {rp}", fh, blocksize=1 << 20)
        sent += os.path.getsize(lp)
        try:
            ftp.sendcmd(f"SITE CHMOD 777 {rp}")
        except ftplib.error_perm:
            pass
        print(f"  {os.path.relpath(lp, local):40s} {sent / total * 100:5.1f}%")
    dt = time.time() - t0
    print(f"  {total / 1e6:.1f} MB in {dt:.1f} s ({total / 1e6 / max(dt, 0.01):.1f} MB/s)")


KNOWN = {"--check", "--confirm-closed", "--confirm-title-unused", "--update"}


ENGINE = os.environ.get("PS5UPLOAD_ENGINE", "http://127.0.0.1:19113")

def http_json(url, body=None):
    req = urllib.request.Request(url, None if body is None else json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.load(e)

def stale_system_ex(ftp, host, tid):
    """True only when every check proves /system_ex/app/<tid> is unmounted, unowned residue."""
    # ponytail: stale empty residue left after a system-UI uninstall; ShadowMount has no
    # supported cleanup for it and its own mount accepts an existing empty directory.
    try:
        path = f"/system_ex/app/{tid}"
        q = urllib.parse.urlencode({"addr": host})
        _, apps = http_json(f"{ENGINE}/api/ps5/apps/installed?{q}")
        _, procs = http_json(f"{ENGINE}/api/ps5/process/list?{q}")
        sm_status, sm = http_json(f"http://{host}:10101/api/v1/games/info", {"title_id": tid})
        _, here = http_json(f"{ENGINE}/api/ps5/fs/stat", {"addr": host, "path": path})
        _, parent = http_json(f"{ENGINE}/api/ps5/fs/stat", {"addr": host, "path": "/system_ex/app"})
        return (apps["incomplete"] is False and apps["registered_unavailable"] is False
                and all(t["title_id"] != tid for t in apps["titles"])
                and procs["truncated"] is False
                and all(p["title_id"] != tid for p in procs["processes"])
                and not exists(ftp, f"/data/homebrew/{tid}") and not exists(ftp, f"/user/app/{tid}")
                and sm_status == 404 and sm["status"] == errno.ENOENT
                and listdir(ftp, path) == []
                and here["kind"] == parent["kind"] == "dir" and here["dev"] == parent["dev"])
    except Exception:
        return False

def guard_remote(ftp, tid, host=None):
    # First install never replaces a mounted directory.
    for root in ("/data/homebrew", "/system_ex/app", "/user/app"):
        if exists(ftp, f"{root}/{tid}") and not (
                root == "/system_ex/app" and host and stale_system_ex(ftp, host, tid)):
            sys.exit(f"collision: {root}/{tid} exists; nothing sent")
    guard_duplicates(ftp, tid)


def guard_duplicates(ftp, tid, skip=None):
    # Detect duplicate IDs even when a homebrew folder has another name.
    for name, is_dir in listdir(ftp, "/data/homebrew"):
        if not is_dir or name == skip:
            continue
        if name in (".", "..") or "/" in name or "\\" in name:
            sys.exit("unsafe remote directory entry; nothing sent")
        # Skip non-app data only after a successful listing proves no sce_sys.
        if not any(entry == "sce_sys" for entry, _ in listdir(ftp, f"/data/homebrew/{name}")):
            continue
        # Unreadable metadata is uncertain, not permission to overwrite.
        data = read_remote(ftp, f"/data/homebrew/{name}/sce_sys/param.json", 65536)
        if json.loads(data)["titleId"] == tid:
            sys.exit(f"duplicate title in /data/homebrew/{name}; nothing sent")


def update_remote(ftp, tid, local):
    """In-place update of existing private smoke title; never deletes."""
    with open(os.path.join(local, "sce_sys/param.json")) as fh:
        mine = json.load(fh)
    if mine.get("titleId") != tid or tid != "NAVI00001":
        sys.exit("local candidate is not NAVI00001; nothing sent")
    remote = f"/data/homebrew/{tid}"
    try:
        theirs = json.loads(read_remote(ftp, f"{remote}/sce_sys/param.json", 65536))
    except (ftplib.Error, ValueError) as e:
        sys.exit(f"remote {remote} param.json missing/unreadable ({e}); nothing sent")
    if not isinstance(theirs, dict) or theirs.get("titleId") != tid or \
            theirs.get("contentId") != mine["contentId"]:
        sys.exit(f"remote {remote} is not this smoke title; nothing sent")
    guard_duplicates(ftp, tid, skip=tid)
    files = local_files(local, remote)
    temps = []
    # Upload everything to temp names first, then promote in order; stop on any error.
    for lp, rp in files:
        d, fn = rp.rsplit("/", 1)
        if d != remote:
            try:
                ftp.mkd(d)
            except ftplib.error_perm:
                pass
        tmp = f"{d}/.{fn}.navi5-tmp"
        with open(lp, "rb") as fh:
            ftp.storbinary(f"STOR {tmp}", fh, blocksize=1 << 20)
        try:
            ftp.sendcmd(f"SITE CHMOD 777 {tmp}")
        except ftplib.error_perm:
            pass
        temps.append(tmp)
        print(f"  staged {os.path.relpath(lp, local)}")
    for i, ((lp, rp), tmp) in enumerate(zip(files, temps)):
        try:
            ftp.rename(tmp, rp)
        except ftplib.Error as e:
            sys.exit(f"promote failed at {rp} ({e}); {i} promoted, "
                     f"temp files left: {', '.join(temps[i:])}. Stopped, no retry.")
    bad = []
    for lp, rp in files:
        with open(lp, "rb") as fh:
            want = hashlib.sha256(fh.read()).hexdigest()
        if hashlib.sha256(read_remote(ftp, rp)).hexdigest() != want:
            bad.append(rp)
    if bad:
        sys.exit("readback SHA-256 mismatch: " + ", ".join(bad))
    print(f"  {len(files)} files verified by SHA-256 readback")


def main():
    args = sys.argv[1:]
    if any(a in ("-h", "--help") for a in args):
        print(__doc__.strip())
        return
    if any(a not in KNOWN for a in args):
        sys.exit("unknown argument: nothing sent")
    update = "--update" in args
    if "--check" in args:
        pass
    elif update and "--confirm-closed" not in args:
        sys.exit("Close Navi5 through PS button, then pass --update --confirm-closed. Nothing sent.")
    elif not update and not {"--confirm-closed", "--confirm-title-unused"} <= set(args):
        sys.exit("Close Navi5 through PS button; verify title unused across console/storage. "
                 "Then pass --confirm-closed --confirm-title-unused. Nothing sent.")
    tid = "NAVI00001"
    local = os.path.join(ROOT, "app/build/app", tid)
    if "--check" not in args:
        import subprocess
        subprocess.run([sys.executable, os.path.join(ROOT, "scripts/check-smoke-package.py"), local], check=True)
    e = env()
    host = os.environ.get("PS5_HOST") or e.get("PS5_HOST")
    if not host:
        sys.exit("PS5_HOST missing in environment/.env.local")
    ftp = connect(host, int(os.environ.get("PS5_FTP_PORT") or e.get("PS5_FTP_PORT", "2121")))
    try:
        if "--check" in args:
            print("FTP answers; this does not prove app closure or title availability")
            return
        if update:
            update_remote(ftp, tid, local)
            print("Updated private smoke candidate in place. Launch manually after lifecycle check.")
            return
        guard_remote(ftp, tid, host)
        staging_root = "/data/navi5-staging"
        staging = f"{staging_root}/{tid}"
        if exists(ftp, staging_root):
            if exists(ftp, staging):
                sys.exit("staging already exists: inspect manually; nothing sent")
        else:
            ftp.mkd(staging_root)
        ftp.mkd(staging)
        upload_tree(ftp, local, staging)
        # Recheck before registration; no launch, deletion or automatic retry.
        guard_remote(ftp, tid, host)
        ftp.rename(staging, f"/data/homebrew/{tid}")
        print("Installed private smoke candidate. Launch manually once only after lifecycle check.")
    finally:
        ftp.quit()


if __name__ == "__main__":
    main()
