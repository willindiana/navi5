#!/usr/bin/env python3
# Navi5 — offline tests for check-pr-test-note.py. SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("note", Path(__file__).with_name("check-pr-test-note.py"))
note = importlib.util.module_from_spec(spec)
spec.loader.exec_module(note)

APP = ["app/src/main.cpp", "docs/QUEUE.md"]
DOCS = ["docs/QUEUE.md", ".github/workflows/host-tests.yml", "app/tests/host/queue.sh"]
FULL = """## Summary
Thing.

## PS5 test
Commit: 1a2b3c4
Build: DEV
Firmware: 13.42
Checked: queue view reorder and remove
Result: pass
"""
TEMPLATE = (Path(__file__).resolve().parents[1] / ".forgejo/pull_request_template.md").read_text()

assert note.app_changed(APP) and not note.app_changed(DOCS) and note.app_changed(["toolkit/x.c"])
assert note.problems(FULL, APP) == []
assert note.problems(FULL, DOCS) == []
assert note.problems("## PS5 test\nN/A: docs only\n", DOCS) == []
assert any("not allowed" in e for e in note.problems("## PS5 test\nN/A: trivial\n", APP))
assert note.problems("## Summary\nno section\n", DOCS) == ['missing "## PS5 test" section (use the PR template)']
# The untouched template counts as empty: its guidance is an HTML comment.
assert note.problems(TEMPLATE, DOCS) == ['write "N/A: <reason>" or a full PS5 test note']
assert len(note.problems(TEMPLATE, APP)) == len(note.FIELDS)
assert any("Result must be pass" in e for e in note.problems(FULL.replace("pass", "fail"), APP))
assert any("Build must be" in e for e in note.problems(FULL.replace("DEV", "emulator"), APP))
assert any("Commit must be" in e for e in note.problems(FULL.replace("1a2b3c4", "latest"), APP))
assert any("missing Firmware" in e for e in note.problems(FULL.replace("Firmware: 13.42\n", ""), APP))
# A later heading ends the section; fields there do not count.
assert note.problems("## PS5 test\n\n## Notes\nResult: pass\n", DOCS) == ['write "N/A: <reason>" or a full PS5 test note']
print("pr test note ok")
