#!/usr/bin/env python3
# Navi5 — offline deployment guard checks; no console contact.
# SPDX-License-Identifier: GPL-3.0-or-later
import ftplib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("deploy", ROOT / "scripts/deploy.py")
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


class FTP:
    def __init__(self, paths=(), duplicate=False, unreadable=False):
        self.paths = paths
        self.duplicate = duplicate
        self.unreadable = unreadable

    def cwd(self, path):
        if path != "/" and path not in self.paths:
            raise ftplib.error_perm("550 not found")

    def retrlines(self, command, callback):
        parent = command.removeprefix("LIST ").rstrip("/")
        for path in self.paths:
            directory, name = path.rsplit("/", 1)
            if directory == parent:
                callback(f"drwxrwxrwx 1 owner group 0 Nov 6 00:00 {name}")
        if parent == "/data/homebrew":
            callback("drwxrwxrwx 1 owner group 0 Nov 6 00:00 other")
        if parent == "/data/homebrew/other":
            # Guard reads param.json only for app folders that list sce_sys.
            callback("drwxrwxrwx 1 owner group 0 Nov 6 00:00 sce_sys")

    def retrbinary(self, command, callback):
        if self.unreadable:
            raise ftplib.error_perm("550 unreadable")
        callback(json.dumps({"titleId": "NAVI00001" if self.duplicate else "TEST00001"}).encode())


def refused(fn):
    try:
        fn()
    except (SystemExit, ftplib.error_perm):
        return
    raise AssertionError("guard allowed unsafe deployment")


for path in ("/data/homebrew/NAVI00001", "/system_ex/app/NAVI00001", "/user/app/NAVI00001"):
    refused(lambda: deploy.guard_remote(FTP(paths=(path,)), "NAVI00001"))
refused(lambda: deploy.guard_remote(FTP(duplicate=True), "NAVI00001"))
refused(lambda: deploy.guard_remote(FTP(unreadable=True), "NAVI00001"))
deploy.guard_remote(FTP(), "NAVI00001")


def stale_http(**bad):
    def http_json(url, body=None):
        if "apps/installed" in url:
            return 200, {"incomplete": bad.get("incomplete", False), "registered_unavailable": False,
                         "titles": [{"title_id": bad.get("registered", "OTHER0001")}]}
        if "process/list" in url:
            return 200, {"truncated": False, "processes": [{"title_id": bad.get("running", "OTHER0001")}]}
        if "10101" in url:
            return bad.get("sm", (404, {"status": 2}))
        if body["path"] == "/system_ex/app":
            return 200, {"kind": "dir", "dev": 213}
        return 200, {"kind": "dir", "dev": bad.get("dev", 213)}
    return http_json


SYS = "/system_ex/app/NAVI00001"
with patch.object(deploy, "http_json", stale_http()):
    deploy.guard_remote(FTP(paths=(SYS,)), "NAVI00001", "console")
    refused(lambda: deploy.guard_remote(FTP(paths=(SYS,)), "NAVI00001"))  # no host: no exception
    for other in ("/data/homebrew/NAVI00001", "/user/app/NAVI00001"):
        refused(lambda: deploy.guard_remote(FTP(paths=(SYS, other)), "NAVI00001", "console"))
    refused(lambda: deploy.guard_remote(FTP(paths=(SYS, SYS + "/eboot.bin")), "NAVI00001", "console"))
    refused(lambda: deploy.guard_remote(FTP(paths=(SYS,), duplicate=True), "NAVI00001", "console"))
for bad in ({"registered": "NAVI00001"}, {"running": "NAVI00001"}, {"incomplete": True},
            {"sm": (200, {"status": 0})}, {"sm": (409, {"status": 16})}, {"dev": 999}):
    with patch.object(deploy, "http_json", stale_http(**bad)):
        refused(lambda: deploy.guard_remote(FTP(paths=(SYS,)), "NAVI00001", "console"))
with patch.object(deploy, "http_json", side_effect=OSError("unreachable")):
    refused(lambda: deploy.guard_remote(FTP(paths=(SYS,)), "NAVI00001", "console"))
for args in ([], ["--yes"], ["--confirm-closed"], ["--confirm-title-unused"], ["--probe"],
             ["--update"], ["--update", "--confirm-title-unused"]):
    with patch.object(sys, "argv", ["deploy.py", *args]), patch.object(deploy, "connect") as connect:
        refused(deploy.main)
        connect.assert_not_called()
with patch.object(deploy, "listdir", side_effect=ftplib.error_perm("550 permission denied")):
    refused(lambda: deploy.guard_remote(FTP(), "NAVI00001"))
with patch.object(deploy, "listdir", return_value=[("bad/name", True)]):
    refused(lambda: deploy.guard_remote(FTP(), "NAVI00001"))
spec = importlib.util.spec_from_file_location("package", ROOT / "scripts/check-smoke-package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)
package.check_ca(ROOT / "app/assets/ca-bundle.pem")
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / "sce_sys").mkdir()
    (path / "sce_sys/param.json").write_text((ROOT / "app/sce_sys/param.json").read_text())
    refused(lambda: package.check_package(path))
    for name in ("eboot.bin", "sce_module/libc.prx", "assets/CA-BUNDLE.md", "assets/ca-bundle-MPL-2.0.txt"):
        file = path / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text("mock")
    (path / "assets/ca-bundle.pem").write_bytes((ROOT / "app/assets/ca-bundle.pem").read_bytes())
    refused(lambda: package.check_package(path))  # no licenses/
    lic = path / "licenses"
    lic.mkdir()
    for src in [ROOT / "LICENSE", ROOT / "THIRD_PARTY_NOTICES.md", ROOT / "app/src/app/CLDR-LICENSE.txt",
                *(ROOT / "app/assets/fonts").glob("*.txt"), *(ROOT / "app/licenses").iterdir()]:
        (lic / src.name).write_bytes(src.read_bytes())
    package.check_package(path)
    (lic / "curl-COPYING.txt").unlink()
    refused(lambda: package.check_package(path))  # one notice missing
    (lic / "curl-COPYING.txt").write_text("stale snapshot")
    refused(lambda: package.check_package(path))  # existing but stale notice
    (lic / "curl-COPYING.txt").write_bytes((ROOT / "app/licenses/curl-COPYING.txt").read_bytes())
    package.check_package(path)
    # Release (template) stays media 65536; marked DEV gets 0 and nothing else changes.
    template = json.loads((ROOT / "app/sce_sys/param.json").read_text())
    assert template["applicationCategoryType"] == 65536
    package.mark_dev(path)
    dev = json.loads((path / "sce_sys/param.json").read_text())
    assert dev == {**template, "applicationCategoryType": 0}
    package.check_package(path)
    (path / package.DEV_MARKER).unlink()
    refused(lambda: package.check_package(path))  # category 0 without DEV marker
    (path / "sce_sys/param.json").write_text(json.dumps(template))
    package.check_package(path)
    (path / package.DEV_MARKER).write_bytes(b"")
    refused(lambda: package.check_package(path))  # marker with media category
    (path / package.DEV_MARKER).unlink()
    (path / "assets/ca-bundle.pem").write_text("invalid")
    refused(lambda: package.check_package(path))
print("offline smoke package/deployment guards passed")
