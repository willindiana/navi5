#!/usr/bin/env python3
# Navi5 — require a PS5 test note in pull requests. SPDX-License-Identifier: GPL-3.0-or-later
"""Offline, read-only. In CI the PR comes from the runner's event file
(GITHUB_EVENT_PATH); PR_BODY, BASE_SHA and HEAD_SHA override it for local runs.
Nothing from the description is ever executed.

A PR that changes app behavior (app/ outside app/tests/, or toolkit/) needs a
"## PS5 test" section with Commit, Build (DEV or release), Firmware, Checked
and "Result: pass". Any other PR may write "N/A: <reason>" there instead."""
import json
import os
import re
import subprocess
import sys

FIELDS = ("Commit", "Build", "Firmware", "Checked", "Result")


def app_changed(paths):
    return any((p.startswith("app/") and not p.startswith("app/tests/")) or p.startswith("toolkit/") for p in paths)


def section(body):
    text = re.sub(r"<!--.*?-->", "", body or "", flags=re.S)
    match = re.search(r"^##\s*PS5 test\s*$(.*?)(?=^##\s|\Z)", text, flags=re.M | re.S | re.I)
    return match.group(1) if match else None


def fields(text):
    found = {}
    for line in text.splitlines():
        m = re.match(r"\s*[-*]?\s*(\w+)\s*:\s*(.*?)\s*$", line)
        if m and m.group(1).capitalize() in FIELDS + ("N/a",):
            found[m.group(1).capitalize()] = m.group(2)
    na = re.search(r"^\s*N/A\s*:\s*(\S.{3,})$", text, flags=re.M | re.I)
    return found, na.group(1).strip() if na else None


def problems(body, paths):
    text = section(body)
    if text is None:
        return ['missing "## PS5 test" section (use the PR template)']
    found, na = fields(text)
    needs_console = app_changed(paths)
    if na and not needs_console and not any(k in found for k in FIELDS):
        return []
    errors = []
    if na and needs_console:
        errors.append("app behavior changed: N/A is not allowed, test on a PS5 (see AGENTS.md)")
    if not needs_console and not na and not found:
        return ['write "N/A: <reason>" or a full PS5 test note']
    for key in FIELDS:
        if not found.get(key):
            errors.append(f"PS5 test: missing {key}:")
    if found.get("Commit") and not re.fullmatch(r"[0-9a-f]{7,40}", found["Commit"]):
        errors.append("PS5 test: Commit must be the installed commit hash")
    if found.get("Build") and found["Build"].lower() not in ("dev", "release"):
        errors.append("PS5 test: Build must be DEV or release")
    if found.get("Result") and found["Result"].lower() != "pass":
        errors.append("PS5 test: Result must be pass before review (keep the PR titled WIP: until then)")
    return errors


def changed_paths(base, head):
    out = subprocess.check_output(["git", "diff", "--name-only", f"{base}...{head}"], text=True)
    return [line for line in out.splitlines() if line]


def pull_request():
    path = os.environ.get("GITHUB_EVENT_PATH")
    if not path:
        return {}
    with open(path) as event:
        return json.load(event).get("pull_request") or {}


if __name__ == "__main__":
    pr = pull_request()
    base = os.environ.get("BASE_SHA") or (pr.get("base") or {}).get("sha", "")
    head = os.environ.get("HEAD_SHA") or (pr.get("head") or {}).get("sha", "")
    body = os.environ["PR_BODY"] if "PR_BODY" in os.environ else pr.get("body") or ""
    if not (re.fullmatch(r"[0-9a-f]{40}", base) and re.fullmatch(r"[0-9a-f]{40}", head)):
        sys.exit("BASE_SHA and HEAD_SHA must be full commit hashes")
    paths = changed_paths(base, head)
    errors = problems(body, paths)
    if errors:
        print("\n".join(f"::error::{e}" for e in errors))
        sys.exit(1)
    print("PS5 test note ok" + (" (app change)" if app_changed(paths) else " (no app change)"))
