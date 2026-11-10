#!/usr/bin/env python3
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Navi5 system backdrop: original abstract sage gradient, no input artwork.
Palette from assets/branding/icon.svg; dark base for system text contrast.
Exports 3840x2160 PNG for sce_sys/pic0/pic1 PNG and DDS. App's existing
blue/violet procedural launch background is independent and unchanged.

    python3 -m venv /tmp/dds && /tmp/dds/bin/pip install numpy pillow etcpak
    /tmp/dds/bin/python app/scripts/make_backdrop.py /tmp/backdrop.png
    /tmp/dds/bin/python app/scripts/make_dds.py /tmp/backdrop.png
"""
import sys

import numpy as np
from PIL import Image

BASE = (16, 24, 20)
# (r, g, b), centre (x, y as fractions of the screen), radius (in screen heights), strength
GLOWS = [
    ((117, 143, 128), (0.04, 0.06), 0.62, 0.34),  # icon #758F80
    ((182, 200, 190), (0.98, 1.02), 0.58, 0.30),  # icon #B6C8BE
]


def render(w, h, seed=7):
    x = (np.arange(w) + 0.5) / w
    y = (np.arange(h) + 0.5) / h
    aspect = w / h
    img = np.zeros((h, w, 3), np.float64)
    img[:] = BASE
    for color, (cx, cy), r, k in GLOWS:
        gx = np.exp(-(((x - cx) * aspect) ** 2) / r**2)   # separable: exp(-(dx²+dy²)/r²)
        gy = np.exp(-((y - cy) ** 2) / r**2)
        img += k * np.outer(gy, gx)[:, :, None] * np.array(color, np.float64)
    # Dither (±0.5 of a step) so the dark gradients never band.
    img += np.random.default_rng(seed).random((h, w, 1)) - 0.5
    return Image.fromarray(np.clip(np.rint(img), 0, 255).astype(np.uint8))


if __name__ == "__main__":
    render(3840, 2160).save(sys.argv[1] if len(sys.argv) > 1 else "backdrop.png")
