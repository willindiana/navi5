#!/usr/bin/env bash
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
# Offline FFmpeg-only rebuild. Fresh work directory; SDK and AV1 cache read-only.
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${1:?usage: build-release-ffmpeg.sh fresh-work-directory}"
[[ ! -e "$WORK" ]] || { echo 'Work directory must not exist' >&2; exit 1; }
# Work/staging stays under permitted project root, never host infrastructure.
python3 - "$WORK" "$ROOT" <<'PY'
from pathlib import Path
import sys
assert Path(sys.argv[1]).resolve().is_relative_to(Path(sys.argv[2]).resolve() / 'build')
PY
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd -P)"
SDK="$(cd "$ROOT/toolchain/ps5-payload-sdk" && pwd -P)"
AV1="$(cd "$ROOT/toolchain/av1-prefix" && pwd -P)"
SOURCE="$ROOT/toolchain/source-cache/ffmpeg-7.1.1.tar.xz"
[[ "$(shasum -a 256 "$SOURCE" | cut -d ' ' -f1)" == 733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1 ]]
eval "$(bash "$ROOT/scripts/setup-toolchain.sh" --env)"
export PATH="$WORK/bin:$SDK/bin:$PATH" NAVI5_FFMPEG_SDK="$SDK" NAVI5_FFMPEG_AV1="$AV1"
mkdir "$WORK/bin"
cat > "$WORK/bin/navi5-pkg-config" <<'EOF'
#!/usr/bin/env bash
set -eu
for arg in "$@"; do
    case "$arg" in dav1d*)
        PKG_CONFIG_SYSROOT_DIR= PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$NAVI5_FFMPEG_AV1/libdata/pkgconfig:$NAVI5_FFMPEG_AV1/lib/pkgconfig" exec pkg-config --static "$@" ;;
    esac
done
exec "$NAVI5_FFMPEG_SDK/bin/prospero-pkg-config" "$@"
EOF
chmod +x "$WORK/bin/navi5-pkg-config"
tar -xJf "$SOURCE" -C "$WORK"
cd "$WORK/ffmpeg-7.1.1"
# Only neutral literal arguments enter runtime *_configuration() strings. SDK
# wrappers resolve real host paths through PATH, not recorded configure arguments.
./configure --prefix=/sdk --enable-cross-compile --cross-prefix=prospero- \
    --enable-static --disable-shared --arch=x86_64 --target-os=freebsd \
    --enable-openssl --enable-version3 --enable-libfreetype --enable-libfribidi \
    --enable-libharfbuzz --enable-libass --disable-debug --disable-doc --disable-programs \
    --enable-libdav1d --disable-decoder=av1 --cc=prospero-clang --cxx=prospero-clang++ \
    --nm=prospero-nm --strip=prospero-strip --ar=prospero-ar --ranlib=prospero-ranlib \
    --pkg-config=navi5-pkg-config > "$WORK/configure.log" 2>&1
python3 - <<'PY'
from pathlib import Path
config = Path('config.h').read_text()
line = next(s for s in config.splitlines() if s.startswith('#define FFMPEG_CONFIGURATION '))
assert '/Users/' not in line and '/home/' not in line and '/private/' not in line
assert '--prefix=/sdk' in line and '--cc=prospero-clang ' in line
for item in ['CONFIG_LIBDAV1D_DECODER 1', 'CONFIG_AV1_DECODER 0', 'HAVE_X86ASM 1', 'HAVE_AVX2_EXTERNAL 1', 'CONFIG_OPENSSL 1', 'CONFIG_LIBASS 1']:
    assert '#define ' + item in config + Path('config_components.h').read_text(), item
PY
make -j4 > "$WORK/build.log" 2>&1
make DESTDIR="$WORK/install" install > "$WORK/install.log" 2>&1
# Reuse unchanged dav1d, headers and licenses on a private delivery/build copy.
cp -R "$AV1" "$WORK/av1-prefix"
cp -R "$WORK/install/sdk/." "$WORK/av1-prefix/"
# pkg-config metadata must point to staging location; archives remain untouched.
python3 - "$WORK/av1-prefix" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
for path in root.rglob('*.pc'):
    text = path.read_text()
    text = '\n'.join('prefix=' + str(root) if line.startswith('prefix=') else line for line in text.splitlines()) + '\n'
    path.write_text(text)
for path in (root / 'lib').glob('*.a'):
    data = path.read_bytes()
    assert not any(marker in data for marker in [b'/Users/', b'/home/', b'/private/']), path.name
print('FFmpeg archives and reused dav1d contain no home/private path markers')
PY
