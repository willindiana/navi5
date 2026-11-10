# Navi5 development

## Build and host tests

Run commands from the repository root unless a step changes directory.

macOS prerequisites:

```sh
brew install llvm lld coreutils bash nasm meson ninja pkgconf
"$(brew --prefix)/bin/bash" scripts/setup-toolchain.sh
cd app
eval "$(../scripts/setup-toolchain.sh --env)"
LC_ALL=C "$(brew --prefix)/bin/bash" scripts/build.sh
```

Linux: use Bash 5 and the Linux dependencies/environment documented in
[scripts/setup-toolchain.sh](../scripts/setup-toolchain.sh); build with `LC_ALL=C`.
Pinned setup uses public open-source ps5-payload SDK v0.43 (not Sony's proprietary SDK), PacBrew v0.39 and FFmpeg 7.1.1/dav1d
1.5.1 by default. SDK/FFmpeg downloads are checksum-pinned; caches stay in ignored
`toolchain/`. Retained video codecs in the toolchain do not enable app video.

Output: `app/build/app/NAVI00001/eboot.bin`; warnings:
`app/build/compile.log`. `--release` also packages `Navi5-<version>.zip`; that
build flag does not make an artifact hardware-validated or publicly releasable.
Release category must remain Media `65536`, with DEV tools/markers excluded.

Offline baseline checks, no server or PS5 required (from the repository root):

```sh
bash app/tests/host/input.sh
bash app/tests/host/shell.sh
```


See [installation](RECIPIENT_RELINK.md#installation-on-supported-setup),
[source and dependency recipes](SOURCE.md) and [manual CI](CI_RELEASE.md).
Use `--release` for Media category 65536; DEV category 0 is not release evidence.
The local reference build used Clang/LLD 23.1.5. Host tests do not prove console behavior.

Never deploy over a running or uncertain process. Close through the PS button,
verify title identity and check all storage for collisions before installation.
Never replace a mounted homebrew directory. `NAVI00001` is unreserved;
`PPSA99505` belongs to Jelly5, never Navi5.

QR pairing uses plaintext HTTP on a trusted LAN; saved passwords are plaintext
on the console. Use Forget before handing the console to someone else.
Report version, firmware/loader, steps and sanitized diagnostics; never post
credentials, QR codes, server URLs, private library metadata or raw captures.

## Controller reference

| Input | Action |
| --- | --- |
| D-pad / Cross | Navigate / select or play |
| Circle | Back; dismiss fullscreen without stopping playback |
| L1 / R1 | Previous / next header view (Home, Artists, Albums, Playlists, Search), wrapping both ways |
| D-pad Up / Down in lists | Scroll continuously; more rows load near the end. Cross retries a failed load |
| Fullscreen Down, Left / Right, Cross | Focus seek, preview in 10-second steps, apply |
| Fullscreen Circle while seeking | Cancel preview before leaving player |
| Fullscreen Left / Right, Cross on Shuffle | Focus shuffle after Next, request live toggle; wait for pending result |
| Square | Backspace anywhere in Search; pause / resume elsewhere, including fullscreen player |
| Touchpad | Stop |
| Triangle | Reopen player from compact playback bar |
| Options | Log out |

Fullscreen controls fade when idle; the first direction or Cross wakes them
without activating. See [gallery/player behavior](GALLERY_PLAYER.md).
