#!/usr/bin/env python3
# Navi5 — offline deployment guard regression tests.
# SPDX-License-Identifier: GPL-3.0-or-later
import ftplib
import json
import os
import tempfile
import unittest
from unittest.mock import Mock, patch

import deploy


class GuardRemoteTests(unittest.TestCase):
    def setUp(self):
        self.ftp = Mock()
        self.listings = {
            "/data": [("homebrew", True)],
            "/system_ex": [("app", True)],
            "/user": [("app", True)],
            "/system_ex/app": [],
            "/user/app": [],
            "/data/homebrew": [("music", True), ("ps5-app-dumper", True)],
            "/data/homebrew/music": [("sce_sys", True)],
            "/data/homebrew/ps5-app-dumper": [
                ("logs", True), ("config.ini", False),
                ("disc_titles.txt", False), ("dumps", True),
            ],
        }
        self.ftp.retrbinary.side_effect = lambda command, collect: collect(
            b'{"titleId":"OTHER0001"}')

    def guard(self):
        def listing(ftp, path):
            result = self.listings[path]
            if isinstance(result, Exception):
                raise result
            return result
        with patch.object(deploy, "listdir", side_effect=listing):
            deploy.guard_remote(self.ftp, "NAVI00001")

    def test_valid_app_and_nonapp_accepted(self):
        self.guard()
        self.ftp.retrbinary.assert_called_once()
        self.assertEqual(self.ftp.retrbinary.call_args.args[0],
                         "RETR /data/homebrew/music/sce_sys/param.json")

    def test_unreadable_directory_refused(self):
        self.listings["/data/homebrew/ps5-app-dumper"] = ftplib.error_perm("550 denied")
        with self.assertRaises(ftplib.error_perm):
            self.guard()

    def test_sce_sys_present_missing_or_unreadable_param_refused(self):
        self.ftp.retrbinary.side_effect = ftplib.error_perm("550 unavailable")
        with self.assertRaises(ftplib.error_perm):
            self.guard()

    def test_malformed_param_refused(self):
        self.ftp.retrbinary.side_effect = lambda command, collect: collect(b"not json")
        with self.assertRaises(json.JSONDecodeError):
            self.guard()

    def test_duplicate_title_refused(self):
        self.ftp.retrbinary.side_effect = lambda command, collect: collect(
            b'{"titleId":"NAVI00001"}')
        with self.assertRaises(SystemExit):
            self.guard()


CID = "UP9000-NAVI00001_00-NAVI500000000000"
REMOTE = "/data/homebrew/NAVI00001"


class FakeFTP:
    """In-memory FTP: files dict, ordered op log, optional readback corruption."""
    def __init__(self, files, corrupt=None):
        self.files, self.ops, self.corrupt = dict(files), [], corrupt

    def retrlines(self, command, callback):
        parent = command.removeprefix("LIST ")
        names = {p[len(parent) + 1:].split("/")[0] for p in self.files if p.startswith(parent + "/")}
        for n in sorted(names):
            callback(f"drwxrwxrwx 1 o g 0 Nov 6 00:00 {n}")

    def retrbinary(self, command, callback):
        path = command.removeprefix("RETR ")
        if path not in self.files:
            raise ftplib.error_perm("550 not found")
        data = self.files[path]
        callback(b"corrupt" if path == self.corrupt and self.ops else data)

    def storbinary(self, command, fh, blocksize=None):
        path = command.removeprefix("STOR ")
        self.ops.append(("STOR", path))
        self.files[path] = fh.read()

    def rename(self, src, dst):
        self.ops.append(("RENAME", src, dst))
        self.files[dst] = self.files.pop(src)

    def mkd(self, path):
        self.ops.append(("MKD", path))

    def sendcmd(self, command):
        pass

    def delete(self, path):
        raise AssertionError("update must never delete")

    rmd = delete


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.local = os.path.join(self.tmp.name, "NAVI00001")
        for rel, data in (("eboot.bin", b"elf"), ("sce_sys/param.json", self.param(CID)),
                          ("sce_sys/icon0.png", b"png"), ("assets/ca-bundle.pem", b"pem"),
                          ("sce_module/libc.prx", b"prx")):
            os.makedirs(os.path.dirname(os.path.join(self.local, rel)), exist_ok=True)
            with open(os.path.join(self.local, rel), "wb") as fh:
                fh.write(data)

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def param(cid, tid="NAVI00001"):
        return json.dumps({"titleId": tid, "contentId": cid}).encode()

    def remote(self, param=None, **extra):
        files = {f"{REMOTE}/eboot.bin": b"old", f"{REMOTE}/user-data.bin": b"keep",
                 "/data/homebrew/PPSA99505/sce_sys/param.json": self.param("JELLY", "PPSA99505"),
                 **extra}
        if param is not None:
            files[f"{REMOTE}/sce_sys/param.json"] = param
        return FakeFTP(files)

    def test_correct_title_updated_in_place(self):
        ftp = self.remote(self.param(CID))
        deploy.update_remote(ftp, "NAVI00001", self.local)
        stores = [op[1] for op in ftp.ops if op[0] == "STOR"]
        renames = [op for op in ftp.ops if op[0] == "RENAME"]
        self.assertTrue(all(p.rsplit("/", 1)[1].endswith(".navi5-tmp") for p in stores))
        self.assertEqual([r[2] for r in renames][-2:],
                         [f"{REMOTE}/eboot.bin", f"{REMOTE}/sce_sys/param.json"])
        for _, src, dst in renames:
            self.assertEqual(src.rsplit("/", 1)[0], dst.rsplit("/", 1)[0])
        # All temps staged before any promotion; no directory replaced.
        self.assertLess(max(i for i, o in enumerate(ftp.ops) if o[0] == "STOR"),
                        min(i for i, o in enumerate(ftp.ops) if o[0] == "RENAME"))
        self.assertNotIn(("MKD", REMOTE), ftp.ops)
        self.assertEqual(ftp.files[f"{REMOTE}/user-data.bin"], b"keep")
        self.assertEqual(ftp.files[f"{REMOTE}/eboot.bin"], b"elf")
        self.assertFalse([p for p in ftp.files if p.endswith(".navi5-tmp")])

    def refused(self, ftp):
        with self.assertRaises(SystemExit):
            deploy.update_remote(ftp, "NAVI00001", self.local)
        self.assertEqual(ftp.ops, [])

    def test_wrong_content_id_refused(self):
        self.refused(self.remote(self.param("UP9000-OTHER")))

    def test_missing_param_refused(self):
        self.refused(self.remote())

    def test_malformed_param_refused(self):
        self.refused(self.remote(b"not json"))

    def test_jelly5_id_refused(self):
        self.refused(self.remote(self.param(CID, "PPSA99505")))
        with self.assertRaises(SystemExit):
            deploy.update_remote(self.remote(self.param(CID)), "PPSA99505", self.local)

    def test_duplicate_elsewhere_refused(self):
        self.refused(self.remote(self.param(CID), **{
            "/data/homebrew/copy/sce_sys/param.json": self.param(CID)}))

    def test_readback_mismatch_fails(self):
        ftp = self.remote(self.param(CID))
        ftp.corrupt = f"{REMOTE}/sce_module/libc.prx"
        with self.assertRaises(SystemExit) as cm:
            deploy.update_remote(ftp, "NAVI00001", self.local)
        self.assertIn("libc.prx", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
