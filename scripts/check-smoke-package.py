#!/usr/bin/env python3
# Navi5 — private smoke artifact checks, offline only.
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CA_SHA = "427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285"


def check_ca(path):
    if hashlib.sha256(path.read_bytes()).hexdigest() != CA_SHA:
        raise SystemExit("CA bundle checksum mismatch")


DEV_MARKER = "NAVI5_DEV_BUILD"  # empty file at package root, DEV builds only

def mark_dev(path):
    """DEV only: game category 0 so Remote Play captures frames; marks package."""
    file = path / "sce_sys/param.json"
    param = json.loads(file.read_text())
    param["applicationCategoryType"] = 0
    file.write_text(json.dumps(param, indent=2, ensure_ascii=False) + "\n")
    (path / DEV_MARKER).write_bytes(b"")

def check_package(path):
    param = json.loads((path / "sce_sys/param.json").read_text())
    # Release (no marker) must stay media 65536; only marked DEV packages use 0.
    category = 0 if (path / DEV_MARKER).is_file() else 65536
    if (param["titleId"] != "NAVI00001" or
            param["contentId"] != "UP9000-NAVI00001_00-NAVI500000000000" or
            param["applicationCategoryType"] != category or param["attribute"] != 536870912):
        raise SystemExit("unexpected smoke identity/category/attribute")
    for name in ("eboot.bin", "sce_module/libc.prx", "assets/CA-BUNDLE.md", "assets/ca-bundle-MPL-2.0.txt"):
        if not (path / name).is_file() or not (path / name).stat().st_size:
            raise SystemExit(f"missing/empty package file: {name}")
    # Every notice and licence text the build stages (app/licenses/README.md).
    sources = [ROOT / "LICENSE", ROOT / "THIRD_PARTY_NOTICES.md"]
    sources += list((ROOT / "app/assets/fonts").glob("*.txt"))
    sources += [p for p in (ROOT / "app/licenses").iterdir() if p.is_file()]
    sources += [ROOT / "app/src/app/CLDR-LICENSE.txt"]
    for source in sources:
        file = path / "licenses" / source.name
        if not file.is_file() or not file.stat().st_size:
            raise SystemExit(f"missing/empty licence file: licenses/{source.name}")
        if file.read_bytes() != source.read_bytes():
            raise SystemExit(f"stale/mismatched licence file: licenses/{source.name}")
    if any(p.is_symlink() for p in path.rglob("*")):
        raise SystemExit("package contains symlink")
    check_ca(path / "assets/ca-bundle.pem")


if __name__ == "__main__":
    if sys.argv[1:] == ["--ca-only"]:
        check_ca(ROOT / "app/assets/ca-bundle.pem")
    elif len(sys.argv) == 3 and sys.argv[1] == "--mark-dev":
        mark_dev(Path(sys.argv[2]))
        print("DEV package: applicationCategoryType 0")
        raise SystemExit(0)
    else:
        check_package(Path(sys.argv[1]) if len(sys.argv) == 2 else ROOT / "app/build/app/NAVI00001")
    print("smoke package check passed")
