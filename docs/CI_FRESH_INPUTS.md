# Fresh two-asset CI preparation

Both manual workflows call `scripts/ci-release.sh`; uploads remain opt-in and
success-only. No release/tag creation. Review pipeline diff before publishing.

## Ready recipe

On fresh Ubuntu 24.04 checkout of **selected, reviewed public commit**:

```sh
sudo apt-get update
sudo apt-get install -y build-essential clang-18 lld-18 llvm-18 ca-certificates curl xz-utils unzip zip python3 python3-venv nasm meson ninja-build pkg-config cmake git
LLVM_CONFIG=llvm-config-18 bash scripts/ci-release.sh
```

Run once in clean worktree with fresh build directories. Preserve old outputs
elsewhere rather than deleting them. No `NAVI5_MODIFIED_FRIBIDI*` inputs needed.
Shared preparation fetches all 77 committed source pins into ignored flat
`toolchain/source-cache`; verified files reused, HTTPS transfers bounded by
connect/transfer/retry limits, downloaded bytes verified before cache replacement.
No new dependencies. Offline source assembly still downloads nothing.

FFmpeg rebuild uses existing neutral `/sdk` configure recipe on private staging
copy. SDK static archives get debug-only stripping on separate `build/ci-sdk`
copy, never shared cache; relink symbols retained. Audited `libcurl.a` stays
byte-for-byte unchanged so app build's original SHA-256 check precedes its symbol
rewrite. Original curl passes recipient privacy markers; no replacement pin.
Live private strings still
fail unchanged recipient/privacy gate. FriBidi extraction checks hash first,
rejects traversal, links, duplicates and special members, then changes only
reviewed version-info string in `lib/fribidi.c`. Cross build uses `-O2 -g0` and
neutral `/sdk` prefix. Matching archive and modified source go to assembler.
Actual unchanged replay, changed-library link, conversion, segment alignment,
imports and SELF integrity remain independently checked; COMPLETE never forced.

Final `build/ci-release` must contain exactly normal ZIP and source ZIP plus
existing packaging metadata as checked by `package-release.py`. Build receipt,
source manifest, source export and package all identify actual checked-out HEAD.
Private history hash is not public history hash: exporting selected public
history changes commit IDs even when files match. Never copy private revision
receipt into public build. Build public checkout itself after separate approval.

## Local validation limits

Local macOS Clang/LLD 23.1.5 can exercise pinned-source build and independent
relink/conversion against existing local capture; that capture's recorded
revision remains historical, not current patch HEAD. This does not validate
fresh Linux LLVM 18 build or assert cross-toolchain bit identity. Existing local
Docker Python 3.12 image supports offline regressions but has no LLVM/compiler/
make. No container/host packages installed for these checks. Fresh complete
Linux LLVM 18 run remains required before declaring two-asset CI ready to ship.

Review follow-up: `test-ci-staging.py` runs actual CI strip command on isolated
copies, then executes app build's actual curl SHA assertion before testing symbol
rewrite and recipient privacy checks. Real original SDK curl and known final
`build/release-sdk` curl both retain audited original hash
`c29815b2e111e12c68975626b7d53418533d0718b18ee229943081068c11527d`.
No blind substitution of stripped curl hash. Both source extractors reject
noncanonical file aliases, duplicate canonical names and links; ordinary tar
directory trailing slash accepted. Focused tests cover both real extractors.
GitHub run `42842807415` read once with explicit `--repo willindiana/navi5`:
completed/failure at public `a305466c9b728649da02f9b27442f1da4c990709`;
not retried or restarted.
