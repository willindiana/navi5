#!/usr/bin/env python3
# Navi5 — release labels, version bumps and the release decision. SPDX-License-Identifier: GPL-3.0-or-later
"""Offline and read-only; uses only the local Git repository (with tags).

    release-version.py check-pr [BASE_SHA HEAD_SHA]  (PR from GITHUB_EVENT_PATH; PR_LABELS overrides)
    release-version.py plan HEAD_SHA                 (Forgejo, on master: should this commit be tagged?)
    release-version.py check-tag TAG SHA BRANCH      (GitHub, on a tag: is it a valid new release?)
    release-version.py notes TAG REPO OUT            (release body with links pinned to the tag)

Every PR to master carries exactly one release label:
  release:major / release:minor / release:patch -> contentVersion in app/sce_sys/param.json
      is the last released version bumped accordingly, and docs/releases/vX.Y.Z.md exists;
  release:skip -> contentVersion unchanged, and master's version is already released.
A push to master is tagged vX.Y.Z when its contentVersion has no tag yet and is above
the last one; the tag (mirrored to GitHub) starts the release build there.
plan, check-tag and check-pr write their results to GITHUB_OUTPUT when set."""
import json
import os
import posixpath
import re
import subprocess
import sys

PARAM = "app/sce_sys/param.json"
LABELS = ("release:major", "release:minor", "release:patch", "release:skip")
TAG = re.compile(r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z.-]+)?$")


def git(*args):
    return subprocess.check_output(["git", *args], text=True)


def parse(content_version):
    if not re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", content_version or ""):
        raise ValueError(f"contentVersion {content_version!r} is not NN.NNN.NNN")
    return tuple(int(p) for p in content_version.split("."))


def tag_of(version):
    return "v%d.%d.%d" % version


def content_version(version):
    return "%02d.%03d.%03d" % version


def version_at(rev):
    return parse(json.loads(git("show", f"{rev}:{PARAM}"))["contentVersion"])


def released(tags):
    """Numeric versions of release tags; prerelease tags count by their numbers."""
    return {tuple(int(g) for g in m.groups()[:3]) for m in map(TAG.match, tags) if m}


def bump(last, label):
    major, minor, patch = last
    return {"release:major": (major + 1, 0, 0), "release:minor": (major, minor + 1, 0),
            "release:patch": (major, minor, patch + 1)}[label]


def check_pr(labels, base, head, released_versions, notes_exist):
    chosen = [name for name in labels if name.startswith("release:")]
    if len(chosen) != 1 or chosen[0] not in LABELS:
        return [f"add exactly one label of {', '.join(LABELS)} (found: {', '.join(chosen) or 'none'})"]
    label, last = chosen[0], max(released_versions, default=(0, 0, 0))
    if label == "release:skip":
        errors = []
        if head != base:
            errors.append(f"release:skip but contentVersion changed {content_version(base)} -> {content_version(head)}")
        if base not in released_versions:
            errors.append(f"master's version {tag_of(base)} is not released yet: this PR needs a release label")
        return errors
    want = bump(last, label)
    errors = []
    if head != want:
        errors.append(f"{label} after {tag_of(last)} needs contentVersion {content_version(want)}, found {content_version(head)}")
    elif not notes_exist(tag_of(head)):
        errors.append(f"missing release notes docs/releases/{tag_of(head)}.md")
    return errors


def plan(head, released_versions, existing_tags):
    tag = tag_of(head)
    if tag in existing_tags or head in released_versions:
        return None
    last = max(released_versions, default=(0, 0, 0))
    if head <= last:
        raise ValueError(f"{tag} is untagged but not above the last release {tag_of(last)}")
    return tag


def release_notes(text, tag, repo, path):
    """Relative links resolve against the notes file; release pages need absolute ones."""
    base = posixpath.dirname(path)

    def absolute(match):
        target = match.group(2)
        if re.match(r"^(https?:|mailto:|#)", target):
            return match.group(0)
        file, _, anchor = target.partition("#")
        resolved = posixpath.normpath(posixpath.join(base, file))
        if resolved.startswith(".."):
            raise ValueError(f"link {target} leaves the repository")
        return f"{match.group(1)}(https://github.com/{repo}/blob/{tag}/{resolved}{'#' + anchor if anchor else ''})"

    return re.sub(r"(\[[^\]]*\])\(([^)\s]+)\)", absolute, text)


def check_tag(tag, version, others, on_branch):
    """A mirrored tag is released only if it names its commit's version, is above
    every other release and points at a commit on master."""
    errors = []
    if tag != tag_of(version):
        errors.append(f"tag {tag} does not match contentVersion {content_version(version)} ({tag_of(version)})")
    if others and version <= max(others):
        errors.append(f"{tag_of(version)} is not above the last release {tag_of(max(others))}")
    if not on_branch:
        errors.append(f"tag {tag} does not point at a commit on master")
    return errors


def event_pr():
    path = os.environ.get("GITHUB_EVENT_PATH")
    if not path:
        return {}
    with open(path) as event:
        return json.load(event).get("pull_request") or {}


def output(lines):
    with open(os.environ.get("GITHUB_OUTPUT") or os.devnull, "a") as out:
        out.write("\n".join(lines) + "\n")


def main(argv):
    tags = git("tag", "--list").split()
    versions = released(tags)
    if argv[1:2] == ["check-pr"] and len(argv) in (2, 4):
        pr = event_pr()
        base, head = argv[2:4] if len(argv) == 4 else ((pr.get("base") or {}).get("sha"), (pr.get("head") or {}).get("sha"))
        if not (base and head):
            raise ValueError("check-pr needs BASE_SHA HEAD_SHA or a pull_request event")
        if "PR_LABELS" in os.environ:
            labels = json.loads(os.environ["PR_LABELS"] or "[]")
        else:
            labels = [label.get("name", "") for label in pr.get("labels") or []]
        exists = lambda tag: subprocess.run(["git", "cat-file", "-e", f"{head}:docs/releases/{tag}.md"],
                                            capture_output=True).returncode == 0
        errors = check_pr(labels, version_at(base), version_at(head), versions, exists)
        for error in errors:
            print(f"::error::{error}")
        if not errors:
            print("release label and version ok")
        return 1 if errors else 0
    if len(argv) == 5 and argv[1] == "notes":
        tag, repo, out = argv[2:]
        if not TAG.match(tag) or not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
            raise ValueError("notes needs a release tag and owner/repo")
        path = f"docs/releases/{tag}.md"
        with open(path) as src, open(out, "w") as dst:
            dst.write(release_notes(src.read(), tag, repo, path))
        return 0
    if len(argv) == 3 and argv[1] == "plan":
        head = version_at(argv[2])
        tag = plan(head, versions, set(tags))
        output([f"release={'true' if tag else 'false'}", f"version={content_version(head)}", f"tag={tag or ''}"])
        print(f"release {tag} ({content_version(head)})" if tag else f"no release: {tag_of(head)} already tagged")
        return 0
    if len(argv) == 5 and argv[1] == "check-tag":
        tag, sha, branch = argv[2:]
        if not TAG.match(tag):
            raise ValueError(f"{tag} is not a release tag")
        version = version_at(sha)
        others = released(t for t in tags if t != tag)
        on_branch = subprocess.run(["git", "merge-base", "--is-ancestor", sha, branch], capture_output=True).returncode == 0
        errors = check_tag(tag, version, others, on_branch)
        for error in errors:
            print(f"::error::{error}")
        if errors:
            return 1
        output([f"version={content_version(version)}", f"tag={tag}"])
        print(f"release {tag} ({content_version(version)})")
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except (ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"::error::{error}")
        sys.exit(1)
