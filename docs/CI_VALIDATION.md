# CI validation: login, saved credentials and identity

Pull requests live on the maintainer's Forgejo (GitHub is a push mirror), so PR
checks are Forgejo workflows (`.forgejo/workflows`); scheduled and manual checks
stay on GitHub (`.github/workflows`). None uses secrets or touches a console.
The RELEASE build workflow is separate: [CI_RELEASE.md](CI_RELEASE.md).

| Workflow | Runs on | Purpose |
|---|---|---|
| `host-tests.yml` (Forgejo) | PR to `master`, push to `master`, manual | Gate: offline checks must pass |
| `navidrome-live.yml` (GitHub) | Mondays 06:17 UTC, manual | Early warning: real Navidrome servers |
| `pr-checks.yml` (Forgejo) | PR to `master` (not `WIP:`), on edit too | Gate: the PR's PS5 test note (`scripts/check-pr-test-note.py`, rules in AGENTS.md) |

## Host checks (`scripts/ci-host-tests.sh`)

Offline, ASan/UBSan where the suite supports it. Same script locally:
`bash scripts/ci-host-tests.sh` (clang, pkg-config, python3, openssl, libcurl and
FFmpeg dev packages).

| Step | Guards |
|---|---|
| `saved_login` | One secret per file (`api_key` or `password`), 0600/0700, atomic replace, malformed files rejected |
| `api_key` | Key format, Navidrome-only issuance, login/create/verify order, header-safe token, rollback |
| `subsonic` | Error 44/42/43 mapping, `tokenInfo`, URL building and redaction |
| `pairing`, `pairing_listener` | Phone form, CSRF/consent, the saved-secret wording shown to users |
| `smoke_worker` | Worker swaps a password for an issued key only where possible; a supplied key is never re-issued |
| `transport_tls` | Certificate/hostname verification, redirect refusal |
| `package_identity`, `deploy_guards`, `stage_dev_login` | DEV `NAVD00001` and PROD `NAVI00001` never mix ID, category, marker or folder; deploy and staging target DEV |
| `workflow_policy` | No PR/tag triggers, pinned actions, no persisted credentials or secrets, hosts in sync |

## Live Navidrome check (`app/tests/host/navidrome_live.py`)

Builds `api_key_live.cpp` on the app's own transport and client code and runs it
against disposable Navidrome containers with a throwaway account:

- `deluan/navidrome:develop`: issue a key, sign in with it alone, `tokenInfo`,
  a library request, unknown key → 44, key plus username → 43, delete the player,
  revoked key → 44, password unaffected. Run over HTTP and again over HTTPS
  through a local TLS front with a one-day CA.
- `deluan/navidrome:latest`: while the release has no API keys, issuance is not
  offered, a forced attempt fails closed and leaves no player behind, and the
  password still works. Once a release advertises keys the script switches this
  server to the full check by itself.

Locally it starts and removes the containers itself (`docker` required);
in CI it uses service containers through `NAVI5_LIVE_DEVELOP_URL` and
`NAVI5_LIVE_STABLE_URL`. It refuses a server that already has other accounts.

This is scheduled rather than a merge gate on purpose: both images move, so a
failure usually means Navidrome changed, not that a commit broke something.
A red run is the signal to re-read the request shapes in `subsonic/api_key.cpp`;
the first green "stable (issue)" run means a stable Navidrome release issues keys.

## Status

Verified on macOS and inside a clean `ubuntu:24.04` container with the two
Navidrome images attached as named containers on one network, which is how a
workflow service container is reached.

Not covered by CI and still manual: anything on PS5 hardware, including the
Media `--release` build's behavior.
