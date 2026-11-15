#!/usr/bin/env python3
# Navi5 — CI workflow policy, offline. SPDX-License-Identifier: GPL-3.0-or-later
"""Every workflow: no pull-request or tag triggers, actions pinned to a commit,
checkout without persisted credentials, no secrets, a job timeout. The validation
workflows exist on both hosts and differ only in how the checkout action is named."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
HOSTS = (".forgejo/workflows", ".github/workflows")
SHARED = ("host-tests.yml", "navidrome-live.yml")
errors = []


def require(ok, message):
    if not ok:
        errors.append(message)


for host in HOSTS:
    for path in sorted((ROOT / host).glob("*.yml")):
        text, name = path.read_text(), f"{host}/{path.name}"
        code = "\n".join(line.split(" #")[0] for line in text.splitlines() if not line.lstrip().startswith("#"))
        require(not re.search(r"\bpull_request(_target)?\b", code), f"{name}: pull-request trigger")
        require(not re.search(r"^\s+tags:", code, re.M), f"{name}: tag trigger")
        require("secrets." not in code, f"{name}: uses secrets")
        require("timeout-minutes:" in code, f"{name}: no job timeout")
        for action in re.findall(r"uses:\s*(\S+)", code):
            require(re.search(r"@[0-9a-f]{40}$", action), f"{name}: action not pinned to a commit: {action}")
        if "checkout@" in code:
            require("persist-credentials: false" in code, f"{name}: checkout keeps credentials")
for shared in SHARED:
    forgejo, github = ((ROOT / host / shared) for host in HOSTS)
    require(forgejo.is_file() and github.is_file(), f"{shared}: missing on one host")
    if forgejo.is_file() and github.is_file():
        require(forgejo.read_text().replace("https://github.com/actions/checkout", "actions/checkout") == github.read_text(),
                f"{shared}: hosts differ beyond the checkout action name")
live = (ROOT / HOSTS[0] / "navidrome-live.yml").read_text()
require("push:" not in live, "navidrome-live.yml: must not gate pushes on a moving upstream image")
if errors:
    sys.exit("\n".join(errors))
print("workflow policy ok")
