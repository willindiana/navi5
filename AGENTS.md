# Navi5 agent instructions

Navi5 is a native Navidrome/Subsonic music client for jailbroken PS5, forked from
02dnot/Jelly5 (`333e393`), GPL-3.0-or-later. Login, QR pairing, saved-login
auto-connect, gapless playback, Play Next and the queue view exist. Preserve
upstream copyrights and licenses. Read the README and the relevant tests first.

This repository is public. Never commit credentials, tokens, server or console
addresses, personal library data or machine-specific paths, and keep them out
of logs, PR text and screenshots.

## Workflow

- Development and PRs happen on the maintainer's self-hosted Forgejo; GitHub is a
  push mirror (never push or open PRs there). `master` is the only long-lived
  branch: work on short `feat/…` or `fix/…` branches and open a PR to `master`.
- Keep commits focused and locally verified. Verify the intended Git author
  before committing.
- Never push, open PRs, deploy to a console or publish without the owner's
  approval for that action.
- Every PR fills in the **PS5 test** section of the PR template (see below).

## Build and offline tests

- Toolchain: `scripts/setup-toolchain.sh`, then from `app/`:
  `eval "$(../scripts/setup-toolchain.sh --env)"` and
  `LC_ALL=C "$(brew --prefix)/bin/bash" scripts/build.sh` on macOS (Bash 5).
  Add `--release` only for the Media build.
- Check `app/build/compile.log`, package integrity and
  `llvm-readelf -d app/build/llvm-pie.elf`: expect `libkernel.sprx`, never
  `libkernel_web` or system-process kernel imports.
- Before every commit run the offline suites: `bash scripts/ci-host-tests.sh`
  (needs no server or console). Run `python3 -B scripts/test-*.py` for packaging
  or CI changes. Clean stale `.d` files after deleting headers.

## Testing on the PS5 while you work

Host tests do not show how a change behaves on a TV, with the real decoder or
against a real server. Any change to app behavior (`app/` outside `app/tests/`,
or `toolkit/`) is checked on a console **before its PR is marked ready**:

1. Build the default (DEV) target. It installs as `NAVD00001` "Navi5 Dev",
   game category, beside the Media title, with its own `/download0` data.
2. Ask the owner to close Navi5 Dev with the PS button, and wait for their
   confirmation. Network silence is not proof the app has closed.
3. Deploy with `scripts/deploy.py --update --confirm-closed` (first install:
   `--confirm-closed --confirm-title-unused`). The console address comes from
   the `PS5_HOST` environment variable; never write it into files.
4. Exercise the changed behavior and its neighbors on the console. Use the
   matching lines of [docs/V1_HARDWARE_CHECKLIST.md](docs/V1_HARDWARE_CHECKLIST.md)
   where they apply. DEV builds allow Remote Play capture, so screenshots and
   input can go through it; use a disposable Navidrome account.
5. Record the result in the PR's **PS5 test** section: commit, build (DEV or
   release), firmware, what was checked and `Result: pass`. If something
   fails, fix it before asking for review. If you could not test, say so and
   keep the PR title prefixed `WIP:`; never claim a check you did not run.

Media-only behavior (background audio, system media controls, the Media
gallery entry) exists only on a `--release` build installed as `NAVI00001`.
Installing it with `scripts/deploy.py --prod` needs separate owner approval
each time. Releases are checked this way before they are published.

Docs-, CI- and script-only PRs write `N/A:` with a one-line reason instead.
The `PR checks` workflow enforces this.

## PS5 safety and platform

- **PPSA99505 is Jelly5's title ID. Never deploy it.** Identities live in
  `scripts/identity.py`: PROD `NAVI00001` "Navi5" (Media, `--release` only) and
  DEV `NAVD00001` "Navi5 Dev" (every other build). Development deploys DEV and
  never touches the installed PROD title. DEV is local-only: never publish,
  reserve or submit it.
- Never deploy over a running or uncertain process. Do not replace mounted
  homebrew directories or create duplicate title IDs.
- Never call `exit()` to close the app. `opendir()` fails in the sandbox; use
  `evo_opendir`.
- Use `jelly5::spawn` / `jelly5::run_all` for async work; handle spawn failure
  and shared lifetime. Keep network, decoding and disk work off the render
  thread. Decide under a mutex, act after releasing it when the action might
  reacquire the lock.
- GPU resources need GPU-visible memory. `evo_direct_mem` may fall back to
  malloc; that fallback must never reach the GPU. Use `gfx_pool`;
  `gfx::texture_release` defers frees.
- Template `param.json` is PROD: Media category 65536, and releases must stay
  65536. Non-release builds stage the DEV title with game category 0 so Remote
  Play capture works; `NAVI5_DEV_TOOLS=1` only adds diagnostics. Keep the
  HDR-capable attribute bit `0x20000000` unchanged without a console test.
- System fonts live under the sandbox word's `common/font/`, not `/preinst`;
  resolve them through `sceKernelGetFsSandboxRandomWord()`. Keep full cJSON 1.7.18.
- Symbolize crash offsets with `llvm-symbolizer --obj=app/build/llvm-pie.elf`.

## Map

- `app/src/gfx/`: GPU UI and artwork; `app/src/ui/`: screens, navigation, focus,
  animation.
- `app/src/app/`: localization, local settings and accounts, safe threading,
  queue and playback session.
- `app/src/subsonic/`: Subsonic/OpenSubsonic client and Navidrome API keys.
- `app/src/jf/jf_http*`: generic HTTP transport only (legacy path); no
  Jellyfin/Emby client or WebSocket.
- `app/src/nuvio_input.c`: scePad; `app/src/ui_text.c`: FreeType/HarfBuzz;
  `app/src/platform/ime.cpp`: system keyboard; `app/engine/`: GPU and PCM audio.

## UI conventions

- Logical canvas 1920×1080, rendered at display resolution. Preserve the glass
  panel, backdrop blur, `ui::Anim`, `ui::Drop`, focus and `art::draw` conventions.
- Viewer-facing strings use `T("…")`: Norwegian source keys, English in
  `app/src/app/i18n.cpp`; translate at use time, never during static
  initialization.
- New source files carry the Navi5 header and
  `SPDX-License-Identifier: GPL-3.0-or-later`.
- Keep FFmpeg audio, sceAudiodec/sceAudioOut, input, keyboard, qrcodegen, cJSON,
  localization and the generic account/settings code. Do not restore video,
  subtitles, bitstream, Jellyfin/Emby, SyncPlay, Seerr or Live TV features.

## Local notes

Machine-specific or private instructions go in `CLAUDE.local.md` (ignored by
Git), never in this file.
