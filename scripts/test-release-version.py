#!/usr/bin/env python3
# Navi5 — offline tests for release-version.py. SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("rv", Path(__file__).with_name("release-version.py"))
rv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rv)

BETA = {"v0.8.0-beta.1"}
released = rv.released(BETA | {"not-a-tag", "v01.0.0"})
assert released == {(0, 8, 0)}
assert rv.parse("01.000.000") == (1, 0, 0) and rv.content_version((1, 2, 3)) == "01.002.003"
assert rv.tag_of((1, 0, 0)) == "v1.0.0"
for bad in ("1.0.0", "01.00.000", "", None):
    try:
        rv.parse(bad)
        raise AssertionError(bad)
    except ValueError:
        pass

yes, no = (lambda tag: True), (lambda tag: False)
v080, v100, v110 = (0, 8, 0), (1, 0, 0), (1, 1, 0)
# v1.0.0 from the beta: major, version bumped (by this PR or an earlier unreleased one), notes present.
assert rv.check_pr(["release:major"], v100, v100, released, yes) == []
assert rv.check_pr(["release:major", "bug"], v080, v100, released, yes) == []
assert "missing release notes" in rv.check_pr(["release:major"], v080, v100, released, no)[0]
assert "needs contentVersion 00.009.000" in rv.check_pr(["release:minor"], v080, v100, released, yes)[0]
assert "needs contentVersion 00.008.001" in rv.check_pr(["release:patch"], v080, v080, released, yes)[0]
# Exactly one known release label.
assert "exactly one" in rv.check_pr([], v080, v080, released, yes)[0]
assert "exactly one" in rv.check_pr(["release:major", "release:skip"], v080, v100, released, yes)[0]
assert "exactly one" in rv.check_pr(["release:huge"], v080, v100, released, yes)[0]
# Skip: version unchanged and master already released.
assert rv.check_pr(["release:skip"], v080, v080, released, yes) == []
assert "changed" in rv.check_pr(["release:skip"], v080, v100, released, yes)[0]
assert "not released yet" in rv.check_pr(["release:skip"], v100, v100, released, yes)[0]
after_v1 = released | {v100}
assert rv.check_pr(["release:minor"], v100, v110, after_v1, yes) == []
assert rv.check_pr(["release:skip"], v100, v100, after_v1, yes) == []

# Plan: release only an untagged version above the last release.
assert rv.plan(v100, released, BETA) == "v1.0.0"
assert rv.plan(v080, released, BETA) is None
assert rv.plan(v100, after_v1, BETA | {"v1.0.0"}) is None
try:
    rv.plan((0, 7, 0), released, BETA)
    raise AssertionError("downgrade")
except ValueError:
    pass
# Release body: relative links pinned to the tag; absolute links and anchors untouched.
body = rv.release_notes("[a](../SAVED_LOGIN.md) [b](../../README.md#controls) [c](https://x.org/y) [d](#z)",
                        "v1.0.0", "owner/navi5", "docs/releases/v1.0.0.md")
assert body == ("[a](https://github.com/owner/navi5/blob/v1.0.0/docs/SAVED_LOGIN.md) "
                "[b](https://github.com/owner/navi5/blob/v1.0.0/README.md#controls) [c](https://x.org/y) [d](#z)"), body
try:
    rv.release_notes("[x](../../../etc/passwd)", "v1.0.0", "o/r", "docs/releases/v1.0.0.md")
    raise AssertionError("escape")
except ValueError:
    pass
# Mirrored tags: must name the commit's version, be newest, and sit on master.
assert rv.check_tag("v1.0.0", v100, released, True) == []
assert "does not match" in rv.check_tag("v1.0.1", v100, released, True)[0]
assert "not above" in rv.check_tag("v0.8.0", v080, released, True)[0]
assert "not point at a commit on master" in rv.check_tag("v1.0.0", v100, released, False)[0]
assert rv.check_tag("v1.1.0", v110, after_v1, True) == []
print("release version ok")
