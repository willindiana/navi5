# Corresponding source

Navi5 v0.8.0-beta.1 binary was built from private revision
`8bde89b36ac5c965e9edf07f4e23eb00cea33d4b`.
This repository is a selected source export, not that revision's original Git history.
Native sources, build scripts, dependency pins and license texts come unchanged
from the matching recipient kit. Operational documents/context are excluded;
public documentation is curated separately. Git IDs will therefore differ.

## Exact release source

Planned destination: <https://github.com/willindiana/navi5/releases/tag/v0.8.0-beta.1>.
This is a proposed publication route, not a claim that a release exists.
The matching `Navi5-v0.8.0-beta.1-recipient-kit.zip` must accompany binary delivery.
GitHub's automatic source archive is not a substitute for this complete kit.

- Recipient kit SHA-256: `9f84fbc1271efcb6823e92f96763d7794035a71c0a79f9578d7bfc45cd3472d4`.
- Unchanged `source/navi5.tar` SHA-256: `9352c75e64de3402883dfe6699471311dd14a5106afc9f25340320dfe374bbda`.
- Normalized `source/relink.tar` SHA-256: `5b9974ab530909ac24be31cd56992d7e31348454dfefb5018eaba8f776528135`.

Kit retains its original source archive, including historical source/build records;
repository documentation curation does not modify or replace that archive.
Kit includes 77 pinned inputs, recipes, modified FriBidi source and relink materials.
Verify kit hash before extracting, then verify supplied checksum lists from their
respective directories. Inspect archive paths before extracting into fresh directories.

## Build recipes

See [development](DEVELOPMENT.md) for tool setup and host tests,
[recipient relink and installation](RECIPIENT_RELINK.md) for library replacement.
The kit's `source/manifest.json` identifies exact inputs and baseline outputs.
Copy its pinned input files into ignored `toolchain/source-cache/` when needed.

- `scripts/source-inputs.json`: 77 immutable/hash-pinned dependency inputs.
- `scripts/setup-toolchain.sh`: SDK v0.43 and PacBrew v0.39 binary tools.
- `scripts/build-av1.sh`: FFmpeg 7.1.1/dav1d 1.5.1 configuration.
- `scripts/build-release-ffmpeg.sh`: offline neutral-prefix FFmpeg build.
- Pinned PacBrew source: full static-library recipes, patches and configuration edits.
- `scripts/build-sdk-runtimes.py`: offline LLVM 18.1.8 runtime recipe.
- `app/scripts/build.sh`: app flags, ordered libraries, stubs, conversion and packaging.
- `scripts/prepare-relink.py`: normalized delivery capture; do not distribute raw capture.

Default build uses FFmpeg 7.1.1/dav1d. `JELLY5_AV1=0` instead selects SDK FFmpeg
7.0.1 and needs separately matched source inputs; do not label it the same build.
Host compiler/linker 23.1.5 differs from SDK LLVM runtime 18.1.8.

```sh
python3 -B app/tests/host/source_delivery.py toolchain/source-cache
python3 -B app/tests/host/llvm_runtime_inputs.py toolchain/source-cache
python3 -B scripts/build-sdk-runtimes.py --cache toolchain/source-cache \
  --sdk "$PS5_PAYLOAD_SDK" --work build/sdk-runtime-rebuild
```

Use CMake 3.20–3.x for the runtime recipe; it retains SDK runtime flags but disables
non-runtime tests/docs/benchmarks. See [generated data](SOURCE_CLDR.md).
Source assembler and CI target their current Git HEAD, not the historical binary;
new builds require new checksums and review. Never substitute newly generated
archives for the exact approved kit above.

## Provenance limits

Retain [third-party notices](../THIRD_PARTY_NOTICES.md), all bundled licenses,
source headers and source distribution recipes. SDK fnmatch matches musl 1.2.5
apart from a commented include; original Rich Felker grant is retained. Header
mapping is in `SDK_INCLUDED_HEADERS.json`; font metadata in `FONT_METADATA.json`.
Historical import revisions and byte-identical upstream SDK reproduction are not
universally established. Relink/conversion does not prove modified-library hardware
behavior, universal privacy or blanket legal clearance.
