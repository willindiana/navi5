"""Navi5 provisional icon check. SPDX-License-Identifier: GPL-3.0-or-later"""
from pathlib import Path
import struct
import xml.etree.ElementTree as ET
import zlib

root = Path(__file__).resolve().parents[2]
svg = ET.parse(Path(__file__).with_name('icon.svg')).getroot()
assert svg.attrib['viewBox'] == '0 0 512 512'
data = (root / 'sce_sys/icon0.png').read_bytes()
assert data[:8] == b'\x89PNG\r\n\x1a\n'
pos, compressed, kinds = 8, [], []
while pos < len(data):
    size, kind = struct.unpack('>I4s', data[pos:pos + 8])
    payload = data[pos + 8:pos + 8 + size]
    assert len(payload) == size
    assert zlib.crc32(kind + payload) == struct.unpack('>I', data[pos + 8 + size:pos + 12 + size])[0]
    if kind == b'IHDR':
        assert struct.unpack('>IIBBBBB', payload) == (512, 512, 8, 2, 0, 0, 0)
    if kind == b'IDAT':
        compressed.append(payload)
    kinds.append(kind)
    pos += size + 12
assert pos == len(data) and kinds[0] == b'IHDR' and kinds[-1] == b'IEND'
assert b'tRNS' not in kinds
pixels = zlib.decompress(b''.join(compressed))
assert len(pixels) == 512 * (1 + 512 * 3)
assert all(pixels[y * 1537] == 0 for y in range(512))
assert len(data) < 2 * 1024 * 1024
print('PASS: SVG canvas; PNG CRCs; 512x512 8-bit RGB color type 2; no alpha/tRNS; complete pixel data; under 2 MiB')

# System backgrounds: verify dimensions/format and paired export identity.
for suffix in ('png', 'dds'):
    bg = (root / f'sce_sys/pic0.{suffix}').read_bytes()
    assert bg == (root / f'sce_sys/pic1.{suffix}').read_bytes()
    if suffix == 'png':
        assert bg[:8] == b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>IIBBBBB', bg[16:29]) == (3840, 2160, 8, 2, 0, 0, 0)
    else:
        assert bg[:4] == b'DDS ' and len(bg) == 148 + 3840 * 2160
        assert struct.unpack_from('<II', bg, 12) == (2160, 3840)
        assert struct.unpack_from('<I', bg, 28)[0] == 1
        assert bg[84:88] == b'DX10'
        assert struct.unpack_from('<IIIII', bg, 128) == (98, 3, 0, 1, 0)
print('PASS: paired 3840x2160 RGB PNG / single-level BC7_UNORM DX10 DDS backgrounds')
