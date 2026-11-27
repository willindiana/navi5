# Relink, modification and installation

Target supported setup: ShadowMount 1.7beta11. No claim for other loader or firmware versions.
`NAVI00001` is unreserved; never use Jelly5 ID `PPSA99505`.

## Recipient modification

1. Download the tag source archive from GitHub (the release page's "Source code"
   link for the release tag) and extract it: that is Navi5's own source, with
   these instructions and every build script. Download
   `Navi5-<version>-third-party-source.zip` from the same release and verify its
   root `SHA256SUMS`, then `source/SHA256SUMS` from `source/`. Its
   `source/manifest.json` `revision` must be the commit the tag points at.
   Extract `source/relink.tar` into a new directory, enter `relink`, run
   `ld.lld @response.txt` with its recorded version. Compare ELF hash with
   supplied baseline before editing anything.
2. Rebuild LGPL FFmpeg/FriBidi/libiconv using the supplied source tarballs (in
   the third-party source ZIP) and the recipes in the tag archive. Find intended
   archive using `llvm-nm` and response ordering; replace only that archive. Keep
   its basename/location. Run `ld.lld @response.txt` again. ABI changes may require
   rebuilding app objects from supplied source. Modification and reverse engineering
   for debugging those modifications are permitted by this delivery.
3. Use tracked `ps5-native-tool link` with `--module-sdk 0x02000009
   --companion-sdk 0x08050001 --file-name eboot.elf`, supplying SDK stub directory
   and generated app stubs exactly as `app/scripts/build.sh` does. Then run
   `ps5-native-tool self --sign --in eboot.elf --out eboot.bin --magic 0x1D3D154F`.
   This is plaintext homebrew fake-SELF signing; no Sony/private signing key needed.
   Preserve 64 KiB read-only segment alignment and inspect imports/SELF integrity.
4. Put changed `eboot.bin` in `app/build/app/NAVI00001/` from matching build.
   Retain `sce_sys/param.json`, `sce_module/libc.prx`, assets and licenses. Validate:

   ```sh
   python3 scripts/check-smoke-package.py app/build/app/NAVI00001
   ```

## Normal release download and CI packaging

Download `Navi5-01.000.001.zip` for installation, not the
`-third-party-source.zip`. Extract into a fresh directory; its only root is
`NAVI00001/`. Follow installation and update precautions below; never replace a
mounted title directory.

To rebuild or relink, begin by downloading the tag source archive from GitHub:
it is Navi5's complete corresponding source for that release (GPL-3.0-or-later),
including build, conversion and packaging scripts. The release no longer
attaches a second copy of the repository.

`Navi5-01.000.001-third-party-source.zip` contains what the tag archive does
not: the 77 pinned third-party source tarballs (FFmpeg, FriBidi, libiconv, SDK
and the rest of `scripts/source-inputs.json`), `relink.tar` (application objects
and link inputs for LGPL relinking), the modified FriBidi library and its
changed source, `manifest.json`, `verification.json` and `SHA256SUMS`. It is not
another install ZIP. Verify root `SHA256SUMS`, then `source/SHA256SUMS` from
`source/`. `DISTRIBUTION.md` and `source/manifest.json` name the exact commit;
the release tag must be that commit.

The tag archive and the export validated during the build have the same files:
`.gitattributes` `export-ignore` leaves out `docs/media`, `app/assets/brand` and
`scripts/__pycache__` from every archive, matching `scripts/assemble-source.py`.
Before publishing, the owner checks the tag against the asset:

```sh
python3 -B scripts/verify-tag-source.py Navi5-01.000.001-third-party-source.zip v1.0.1
```

It fails unless the tag is the recorded commit and `git archive` of it has the
digest of the validated export. GitHub wraps the same files in its own
tar.gz/zip container, so archive bytes differ from that digest by design.

Trusted Forgejo workflows run `scripts/ci-release.sh` against
current checked-out HEAD. Full validated delivery remains in `build/ci-delivery`;
`build/ci-release` contains exactly the normal and third-party source ZIPs. Source gate and
independent recipient replay run before packaging. CI logs final asset names,
sizes and SHA-256 hashes even when `upload_artifacts=false` (default).
`DISTRIBUTION.md` in new CI third-party source ZIPs maps original mixed-root build ZIP to
folder-only distributed ZIP; unchanged `validation.json` records original build
hashes and actual CI revision. Existing published source kits use
`SOURCE_IDENTITY.md` for their original/distributed mapping.

Packaging copies native-folder bytes and metadata, omitting root image and
redundant root licenses. No binary rebuild happens during packaging. Same recipe
and checks are reproducible; byte-identical binaries across LLVM versions or
host toolchains are not promised. Offline packaging regression:

```sh
python3 -B scripts/test-package-release.py
python3 -B scripts/test-verify-tag-source.py
```

The delivery gate is unchanged: `build/ci-delivery` still holds the full kit,
including the repository export it was checked against. Only the public asset
drops that duplicate. `docs/SHIP_OBLIGATIONS.md` remains the reference for what
must be conveyed.

## Installation on supported setup

FTP is unencrypted. Use a trusted LAN. Commands below perform console writes;
they are instructions only, not authorization to run them in this session.
Set `PS5_HOST` to your own console address locally; never publish `.env.local`.

1. Close Navi5 through the PS button and verify it is no longer running. Confirm
   ShadowMount **1.7beta11** and title identity. Stop on uncertain lifecycle state.
2. For an existing matching `/data/homebrew/NAVI00001`, verify remote title/content
   IDs match local `param.json`, no duplicate across title database/storage, and
   preserve backup outside loader scan roots. Update files **in place**; do not
   replace/delete/rename the mounted title directory. Run from source root:

   ```sh
   python3 scripts/deploy.py --update --confirm-closed
   ```

   Script checks identity, stages temporary files, promotes executable/metadata
   last and verifies SHA-256 readback. Failure may leave partial promotion: stop,
   inspect, do not launch or automatically retry. Closure flag is human assertion,
   not independent proof. This does not prove rollback or transactional update.
3. First install only: verify `NAVI00001` absent from title database and every
   attached storage/scan root; confirm no conflicting mounted title. Then:

   ```sh
   python3 scripts/deploy.py --confirm-closed --confirm-title-unused
   ```

   Script stages outside scan root then registers folder. Missing safety evidence
   stops installation; do not bypass checks or reuse Jelly5's title ID.
4. Wait for loader registration/readiness, launch manually once. Modified-library
   relink/conversion is not runtime acceptance. Recheck closure before next update.
