#!/usr/bin/env python3
# Navi5 — CI workflow policy, offline. SPDX-License-Identifier: GPL-3.0-or-later
"""Every workflow: no pull_request_target or tag triggers, read-only contents for
pull-request workflows, actions pinned to a commit, checkout without persisted
credentials, no secrets, a job timeout. Workflows live on GitHub only; the Forgejo
copy of the repository is a pull mirror and runs none."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
HOSTS = (".github/workflows",)
errors = []


def require(ok, message):
    if not ok:
        errors.append(message)


for host in HOSTS:
    for path in sorted((ROOT / host).glob("*.yml")):
        text, name = path.read_text(), f"{host}/{path.name}"
        code = "\n".join(line.split(" #")[0] for line in text.splitlines() if not line.lstrip().startswith("#"))
        require(not re.search(r"\bpull_request_target\b", code), f"{name}: pull_request_target trigger")
        if re.search(r"\bpull_request\b", code):
            require(re.search(r"^permissions:\n\s+contents: read\s*$", code, re.M), f"{name}: pull-request workflow without read-only contents")
        require(not re.search(r"^\s+tags:", code, re.M), f"{name}: tag trigger")
        require("secrets." not in code, f"{name}: uses secrets")
        require("timeout-minutes:" in code, f"{name}: no job timeout")
        for action in re.findall(r"uses:\s*(\S+)", code):
            require(re.search(r"@[0-9a-f]{40}$", action), f"{name}: action not pinned to a commit: {action}")
        if "checkout@" in code:
            require("persist-credentials: false" in code, f"{name}: checkout keeps credentials")
require(not (ROOT / ".forgejo/workflows").exists(), ".forgejo/workflows: Forgejo is a pull mirror and must not run workflows")
live = (ROOT / HOSTS[0] / "navidrome-live.yml").read_text()
require("push:" not in live, "navidrome-live.yml: must not gate pushes on a moving upstream image")
if errors:
    sys.exit("\n".join(errors))
print("workflow policy ok")
