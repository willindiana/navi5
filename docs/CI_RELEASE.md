# Master RELEASE validation

Owner approved `master` as default, separate from private repository history.
Existing `release/beta`, old branches, tags and frozen `v0.8.0-beta.1` assets stay
unchanged. This approval covers workflow activation and native protections, not
publication, artifact uploads, store submissions or infrastructure changes.

GitHub runs full `scripts/ci-release.sh` on every master push/merge and PR targeting
master, including docs changes. GitHub-hosted Ubuntu 24.04 runners use LLVM 18,
`contents: read`, SHA-pinned actions and `persist-credentials: false`. No secrets,
publisher, release/tag event or scheduled release. Manual dispatch targets master;
artifact upload defaults to false and true needs separate owner approval.
Forgejo PR execution stays disabled: persistent privileged runners must not run
untrusted PR code. Without verified safe pre-merge checks, Forgejo cannot claim
passing CI before merge; post-merge builds do not supply that guarantee.

Native master protections must require PRs, passing observed exact CI contexts,
up-to-date branches, no direct/force pushes or deletion, no configured admin
bypass. Solo owner requires zero mandatory second-reviewer approvals. Verify live
settings and successful new-context runs separately; workflow files prove neither.
Administrators can still edit repository settings.

## Separate hardware and publication gate

CI validates COMPLETE corresponding source/relink delivery and packages exactly
two ZIPs; it does not establish hardware acceptance. No automatic publication.
Before public publishing, owner must approve exact commit, unused tag, increasing
numeric contentVersion, both ZIP names/hashes and hardware test receipt for exact
install bytes. Promote those frozen bytes and matching COMPLETE source, never
silently rebuild. Never clobber, overwrite or recreate existing tags/assets.

Ubuntu 24.04/LLVM 18 CI differs from local Clang/LLD 23.1.5. New outputs do not
replace frozen TV-tested beta bytes. Artifact upload true needs separate approval;
seven-day CI storage is not durable corresponding-source delivery.

## Read-only deployed store verification

Manual-only `.github/workflows/store-verify.yml` targets master. Supply exact tag,
numeric contentVersion and install ZIP SHA-256 from independently approved frozen
receipt, never from live catalog or latest release. Hosted runner uses read-only
checkout, no secrets and no artifact download/upload. Equivalent local command:

```sh
python3 -B scripts/check-store-release.py --tag "$APPROVED_TAG" \
  --content-version "$APPROVED_CONTENT_VERSION" --sha256 "$APPROVED_INSTALL_SHA256" \
  --attempts 20 --interval 60
```

Optional `--install /path/to/Navi5-VERSION.zip` checks against expected hash. Exit
0 means catalog identity verified, 2 means pending/unavailable/unsupported schema;
invalid invocation fails. Check is not install/runtime acceptance. Pending never
justifies republishing; rerun read-only later. No automatic store submission.
RecompStore verification remains pending approval, disabled and nonblocking until
approved public listing and supported version/install-asset contract exist.

Offline checks: `python3 -B scripts/test-store-release.py`,
`python3 -B scripts/test-ci-release.py`, `ruby scripts/test-workflow-policy.rb`,
`bash -n scripts/ci-release.sh`, `git diff --check`.

See [source identity](SOURCE.md), [build](DEVELOPMENT.md) and
[installation](RECIPIENT_RELINK.md#installation-on-supported-setup).
