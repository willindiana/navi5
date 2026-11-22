#!/usr/bin/env python3
# Navi5 — CI workflow policy, offline. SPDX-License-Identifier: GPL-3.0-or-later
"""Every workflow: no pull_request_target or tag triggers, read-only contents for
pull-request workflows, actions pinned to a commit, checkout without persisted
credentials, a job timeout, and no secrets except the shared package token
(Forgejo only, for the CI image registry login). Pull requests live on Forgejo, so the PR
checks are Forgejo workflows; GitHub is a push mirror and runs no PR workflows."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
HOSTS = (".forgejo/workflows", ".github/workflows")
PR_CHECKS = ("host-tests.yml", "release-build.yml")
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
        for line in code.splitlines():
            if "secrets." in line:
                allowed = host == HOSTS[0] and re.fullmatch(
                    r"\s+(CI_TOKEN|password): \$\{\{ secrets\.PACKAGES_TOKEN \}\}", line)
                require(allowed, f"{name}: secret outside the CI image registry login: {line.strip()}")
        require("timeout-minutes:" in code, f"{name}: no job timeout")
        require("docker.sock" not in code, f"{name}: host Docker socket")
        for action in re.findall(r"uses:\s*(\S+)", code):
            require(re.search(r"@[0-9a-f]{40}$", action), f"{name}: action not pinned to a commit: {action}")
        if "checkout@" in code:
            require("persist-credentials: false" in code, f"{name}: checkout keeps credentials")
for name in PR_CHECKS:
    path = ROOT / HOSTS[0] / name
    require(path.is_file() and re.search(r"^  pull_request:", path.read_text(), re.M), f"{HOSTS[0]}/{name}: must check pull requests")
for path in sorted((ROOT / HOSTS[1]).glob("*.yml")):
    require(not re.search(r"\bpull_request\b", path.read_text()), f"{HOSTS[1]}/{path.name}: PRs are checked on Forgejo, not GitHub")
live = (ROOT / HOSTS[1] / "navidrome-live.yml").read_text()
require("push:" not in live, "navidrome-live.yml: must not gate pushes on a moving upstream image")
if errors:
    sys.exit("\n".join(errors))
print("workflow policy ok")
