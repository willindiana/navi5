#!/usr/bin/env python3
# Navi5 — SPDX-License-Identifier: GPL-3.0-or-later
"""Offline recipe guard; optional real archive/configuration check after rebuild."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
recipe = root / 'scripts/build-release-ffmpeg.sh'
subprocess.run(['bash', '-n', str(recipe)], check=True)
text = recipe.read_text()
command = text[text.index('./configure --prefix='):text.index("python3 - <<'PY'", text.index('./configure --prefix='))]
assert '--prefix=/sdk' in command and '--pkg-config=navi5-pkg-config' in command
assert '${' not in command and '$SDK' not in command and '$AV1' not in command
assert 'make DESTDIR="$WORK/install" install' in text
assert 'curl ' not in text and 'wget ' not in text
if len(sys.argv) == 2:
    work = Path(sys.argv[1])
    config = (work / 'ffmpeg-7.1.1/config.h').read_text()
    line = next(s for s in config.splitlines() if s.startswith('#define FFMPEG_CONFIGURATION '))
    assert '--prefix=/sdk' in line and '--cross-prefix=prospero-' in line
    for marker in ['/Users/', '/home/', '/private/']:
        assert marker not in line
        for archive in (work / 'av1-prefix/lib').glob('*.a'):
            assert marker.encode() not in archive.read_bytes(), archive.name
    assert (work / 'av1-prefix/lib/libavcodec.a').stat().st_size > 1000000
print('neutral FFmpeg recipe and optional built archive checks passed')
