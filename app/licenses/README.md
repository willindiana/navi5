# Licence texts shipped with Navi5

2026-11-09 source-closure delta: `openlibm-LICENSE.md` and
`openlibm-NOTICES.txt` preserve root/per-file notices from pinned 0.8.6 source;
`libjpeg-turbo-README.ijg.txt` retains complete 3.0.2 IJG README;
`libpsl-punycode-NOTICE.txt` retains Costello fallback grant from 0.21.5 `src/psl.c`.
`musl-COPYRIGHT.txt` is verbatim musl 1.2.5 grant; SDK fnmatch matches except
commented locale include. `sdk-header-NOTICES.txt` covers 99 byte-matched SDK
headers. Details and remaining limits: [SOURCE_CLOSURE.md](../../docs/SOURCE_CLOSURE.md).
Older fnmatch/header findings below are historical, not current missing grants.

The build copies this folder, the root `LICENSE`, `THIRD_PARTY_NOTICES.md`,
`app/assets/fonts/*.txt` and `app/src/app/CLDR-LICENSE.txt` into the app's
`licenses/` folder (`app/scripts/build.sh`); `scripts/check-smoke-package.py`
fails a package that lacks any of them. Texts are verbatim upstream copies;
none are paraphrased. Each was compared byte-for-byte with the source below
on 2026-11-09. Versions come from the pinned SDK v0.43 / PacBrew v0.39 sysroot
metadata (`scripts/setup-toolchain.sh`) and `scripts/build-av1.sh`.

| File | Component and version | Verbatim source |
| --- | --- | --- |
| `Babel-LICENSE.txt` | Babel 2.18.0 generator input | pinned source distribution `LICENSE`; CLDR license separately retained, see [SOURCE_CLDR.md](../../docs/SOURCE_CLDR.md) |
| `pycountry-LICENSE.txt` | pycountry 24.6.1 generator input | pinned source distribution `LICENSE.txt` (LGPL-2.1-only per package metadata) |
| `bzip2-LICENSE.txt` | bzip2 1.0.8 | sourceware bzip2.git `LICENSE` at tag `bzip2-1.0.8` |
| `ca-bundle-MPL-2.0.txt` | Mozilla CA data (see `app/assets/CA-BUNDLE.md`) | MPL-2.0 text, unchanged from earlier commit |
| `cJSON-LICENSE.txt` | cJSON 1.7.18 | `app/engine/addons/src/cJSON-LICENSE`; identical to DaveGamble/cJSON `v1.7.18` `LICENSE` |
| `curl-COPYING.txt` | curl/libcurl 8.18.0 | curl/curl `COPYING` at tag `curl-8_18_0` |
| `dav1d-COPYING.txt` | dav1d 1.5.1 | `COPYING` from pinned `dav1d-1.5.1.tar.xz` (SHA-256 in `scripts/build-av1.sh`) |
| `FFmpeg-COPYING.LGPLv3.txt`, `FFmpeg-LICENSE.md` | FFmpeg 7.1.1 (default) and 7.0.1 (`JELLY5_AV1=0`) | from pinned `ffmpeg-7.1.1.tar.xz`; identical to FFmpeg `n7.0.1` files |
| `freetype-LICENSE.txt`, `freetype-FTL.txt` | FreeType 2.13.2 | freetype/freetype `LICENSE.TXT`, `docs/FTL.TXT` at tag `VER-2-13-2` |
| `fribidi-COPYING.txt` | FriBidi 1.0.15 | fribidi/fribidi `COPYING` at tag `v1.0.15` |
| `harfbuzz-COPYING.txt` | HarfBuzz 9.0.0 | harfbuzz/harfbuzz `COPYING` at tag `9.0.0` |
| `libiconv-COPYING.LIB.txt` | GNU libiconv 1.17 | savannah libiconv.git `COPYING.LIB` at tag `v1.17` |
| `libjpeg-turbo-LICENSE.md` | libjpeg-turbo 3.0.2 | libjpeg-turbo `LICENSE.md` at tag `3.0.2`; same as SDK `share/doc/libjpeg-turbo/LICENSE.md` |
| `libpng-LICENSE.txt` | libpng 1.6.43 | pnggroup/libpng `LICENSE` at tag `v1.6.43` |
| `libpsl-COPYING.txt` | libpsl 0.21.5 | rockdaboot/libpsl `COPYING` at tag `0.21.5` (same as its `LICENSE`) |
| `libpsl-LICENSE.chromium.txt` | libpsl 0.21.5 Chromium lookup code | rockdaboot/libpsl `src/LICENSE.chromium` at tag `0.21.5` |
| `libwebp-COPYING.txt` | libwebp/libsharpyuv 1.4.0 | webmproject/libwebp `COPYING` at tag `v1.4.0` |
| `libxml2-Copyright.txt` | libxml2 2.13.5 | GNOME/libxml2 `Copyright` at tag `v2.13.5` |
| `LiquidGlass-LICENSE.txt` | OverShifted/LiquidGlass (shader derivation) | `LICENSE` at commit `3797fa541c1f026c521a75885ee271f03bdf9f0f` (file's only commit); derivation revision unknown |
| `llvm-libcxx-LICENSE.txt`, `llvm-libcxxabi-LICENSE.txt`, `llvm-libunwind-LICENSE.txt` | LLVM runtimes 18.1.8 | llvm-project `{libcxx,libcxxabi,libunwind}/LICENSE.TXT` at `llvmorg-18.1.8`; SDK v0.43 `libcxx.sh` pins 18.1.8, sysroot `_LIBCPP_VERSION 180100` |
| `nanosvg-LICENSE.txt` | NanoSVG (revision unknown) | `app/third_party/nanosvg/LICENSE.txt`, identical to memononen/nanosvg `master` today |
| `openssl-LICENSE.txt` | OpenSSL 3.5.2 | openssl/openssl `LICENSE.txt` at tag `openssl-3.5.2` (no NOTICE file at that tag) |
| `qrcodegen-LICENSE.txt` | Project Nayuki QR Code generator (revision unknown) | lines 1–22 of retained `app/src/qrcodegen.c` |
| `xz-COPYING.txt` | xz/liblzma 5.4.6 (liblzma public domain in this version) | tukaani-project/xz `COPYING` at tag `v5.4.6`; same as SDK `share/doc/xz/COPYING` |
| `zlib-LICENSE.txt` | zlib 1.3.2 | madler/zlib `LICENSE` at tag `v1.3.2` |
| `zstd-LICENSE.txt` | Zstandard 1.5.6 | facebook/zstd `LICENSE` at tag `v1.5.6` |

Notes: referenced GPLv3 text that LGPLv3 builds on is root `LICENSE`.
`GPL-2.0.txt` is verbatim `libiconv-1.17/COPYING` from the verified source
archive in `scripts/source-inputs.json`; supplies GPLv2 referenced by LGPLv2.1.
Additional records: `PSL-SOURCE.txt` pins the built-in list to the verified
libpsl 0.21.5 release source member. `sdk-libc-NOTICES.txt` preserves verbatim
leading comments from every C/assembly/companion header in SDK v0.43 `libc/`
at commit `d9c9519116944a7f1c22d262012c53b80b3520b7`, archive SHA-256
`44b541e77eef201afb4fbb6b32af9b97a4cdf893fc2d944d228ff46a8235fd10`:
https://codeload.github.com/ps5-payload-dev/sdk/tar.gz/d9c9519116944a7f1c22d262012c53b80b3520b7
This is conservative source-notice coverage, not an assertion that every member
is linked or that source/object reproducibility was proven. Included SDK FreeBSD
headers may contribute additional terms; final extracted-member/header mapping
remains open. `fnmatch.c` attributes Rich Felker but omits a full grant; its
original import/license tracing remains open. Do not infer proprietary Sony code.
Roboto metadata/copyright now recorded in `docs/FONT_METADATA.json`; exact
upstream file match and source NOTICE still unverified.
