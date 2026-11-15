# Release builds and publication

Development and pull requests happen on the maintainer's self-hosted Forgejo.
It push-mirrors every branch and tag to GitHub, which publishes releases. All
workflows use SHA-pinned actions, `persist-credentials: false` and no stored
secrets. The Forgejo repository is private and only the maintainer's own
branches open PRs there; its runner never executes code from outside contributors.

## Every pull request (Forgejo)

- `host-tests.yml`: the offline suites (`scripts/ci-host-tests.sh`).
- `release-build.yml`: the full `scripts/ci-release.sh` RELEASE build with the
  source delivery gate, on every PR and every push to `master`.
- `pr-checks.yml`, skipped while the title starts with `WIP:`:
  - **PS5 test note**: app-behavior changes need a console test with
    `Result: pass` (see [AGENTS.md](../AGENTS.md)).
  - **Release label**: exactly one of `release:major`, `release:minor`,
    `release:patch`, `release:skip`. For the first three, `contentVersion` in
    `app/sce_sys/param.json` must be the last released version bumped that way
    and `docs/releases/vX.Y.Z.md` must exist. `release:skip` keeps the version
    unchanged, and is refused while `master` carries an unreleased version.

## Merge to master

1. **Tag (Forgejo, `tag-release.yml`)**: when `contentVersion` has no tag yet and
   is above the last release (`scripts/release-version.py plan`), the merge commit
   is tagged `vX.Y.Z` through the Forgejo API. The tag is created on Forgejo
   because the push mirror makes GitHub an exact copy: a tag created only on
   GitHub would be removed by the next sync.
2. The push mirror brings the tag to GitHub (on push, and at least every 10 minutes).
3. **Release (GitHub, `release.yml`)**, on the `v*` tag:
   - **plan**: the tag must name its commit's `contentVersion`, be above every
     other release and point at a commit on `master` (`release-version.py check-tag`).
   - **build**: `scripts/ci-release.sh`, which must produce exactly
     `Navi5-<contentVersion>.zip` and `Navi5-<contentVersion>-third-party-source.zip`.
     Both are kept as the run artifact `Navi5-vX.Y.Z` for 30 days; their SHA-256
     sums are in the run summary.
   - **publish** waits for approval of the protected `release` environment. This
     is the hardware gate: download the run artifact, install
     `Navi5-<contentVersion>.zip` as `NAVI00001` (`scripts/deploy.py --prod`, after
     closing the app), run the release lines of
     [V1_HARDWARE_CHECKLIST.md](V1_HARDWARE_CHECKLIST.md), then approve. These exact
     bytes are published; nothing is rebuilt. The job refuses an environment
     without required reviewers and an existing release, checks the tag is the
     built commit, runs `scripts/verify-tag-source.py`, and creates the GitHub
     release on the existing tag with both ZIPs and `docs/releases/vX.Y.Z.md` as
     notes (links pinned to the tag).
   - **verify**: the published assets must hash to the tested artifact. A store
     catalog check runs for information; "pending" is normal until the catalog
     PR is merged.

If the PS5 test fails, reject the deployment. The tag stays without a release,
and the fix ships as the next patch version. Tags, releases and assets are never
moved, replaced, deleted or clobbered. `scripts/test-workflow-policy.py` enforces
these properties of both workflows.

Repository settings this relies on:

- Forgejo: `master` protected (no direct pushes, also for admins; PRs must be up
  to date), required status checks from the workflows above, labels
  `release:major|minor|patch|skip`, Actions enabled, a push mirror to GitHub.
- GitHub: `master` and tags only receive the mirror (no force pushes or
  deletion of `master`), environment `release` with the owner as required
  reviewer and deployments limited to `v*` tags.

## Read-only deployed store verification

Manual-only `.github/workflows/store-verify.yml` and the local command check a
store catalog against an exact tag, numeric contentVersion and install ZIP
SHA-256 taken from the release, never from the live catalog:

```sh
python3 -B scripts/check-store-release.py --tag "$TAG" \
  --content-version "$CONTENT_VERSION" --sha256 "$INSTALL_SHA256" \
  --attempts 20 --interval 60
```

Exit 0 means the catalog identity is verified, 2 means pending, unavailable or an
unsupported schema. Pending never justifies republishing: rerun later. Store
submissions are separate, owner-approved catalog PRs.

## Packaging and source

`build/ci-release/` holds exactly the two ZIPs, with matching build/source
identity and checksums. Navi5's own source is the release tag's automatic source
archive; the third-party asset records the commit that tag must be, which
`scripts/verify-tag-source.py` checks before publishing. See
[RECIPIENT_RELINK.md](RECIPIENT_RELINK.md), [source identity](SOURCE.md) and
[build](DEVELOPMENT.md).

Offline checks: `python3 -B scripts/test-release-version.py`,
`python3 -B scripts/test-store-release.py`, `python3 -B scripts/test-ci-release.py`,
`python3 -B scripts/test-workflow-policy.py`, `ruby scripts/test-workflow-policy.rb`,
`bash -n scripts/ci-release.sh`.
