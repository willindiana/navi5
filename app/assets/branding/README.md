# Provisional Navi5 icon

`icon.svg` is editable source for `../../sce_sys/icon0.png`: original geometric
N built from two note stems, diagonal and note heads, in smoked-sage glass.
User requested this provisional app icon after reviewing concept 05. AI-assisted,
code-drawn artwork, not a commissioned human designer's final. No external
images, fonts, copied logos or image-generation API. Human-designed final remains
preferred; no claim of exclusive copyright, trademark clearance or endorsement.
Repository GPL-3.0-or-later distribution terms apply to this contribution to the
extent rights exist; no invented individual copyright/authorship assertion.

Geometry matches concept 05 unchanged. macOS Quick Look rendered source at 512;
Python standard library removed alpha losslessly from fully opaque RGBA preview,
retaining RGB pixel bytes. Output is 512x512, 8-bit RGB PNG (color type 2), no
alpha or tRNS. Quick Look rendering may vary by macOS version. Review new exports
before replacing committed PNG. Check committed export:

```sh
python3 app/assets/branding/check.py
sips -g pixelWidth -g pixelHeight -g samplesPerPixel -g hasAlpha app/sce_sys/icon0.png
```

512/128/64 previews were visually reviewed: N and note heads remain distinct;
no clipping; darkest mark versus dimmest outer-24px background contrast 6.75:1
(simple preview heuristic, not accessibility certification or console test).
## System backgrounds

`../../scripts/make_backdrop.py` is editable source for original abstract sage
backgrounds, using only numeric colors `#758F80`, `#B6C8BE`, dark base
`(16, 24, 20)` and procedural Gaussian glows/dither (seed 7). No input image,
font, logo, trademark or inherited artwork. Existing GPL renderer code retained;
new palette composition is AI-assisted code-drawn work, not third-party generated
imagery. Same rights caveats and GPL distribution terms as icon above.

`../../sce_sys/pic0.png` and `pic1.png` are identical 3840×2160 RGB exports.
`../../scripts/make_dds.py` encodes them as single-level BC7_UNORM DX10 DDS.
Export environment: Python 3.14, NumPy 2.5.4, Pillow 13.0.0, etcpak 0.9.15.
Run from repository root with these dependencies available:

```sh
python3 app/scripts/make_backdrop.py app/sce_sys/pic0.png
cp app/sce_sys/pic0.png app/sce_sys/pic1.png
python3 app/scripts/make_dds.py app/sce_sys/pic0.png
python3 app/assets/branding/check.py
```

960×540 preview visually reviewed: plain dark sage glows, no text or inherited
art. Console/system-text legibility remains untested. Existing in-app procedural
blue/violet background unchanged; no wider branding/UI changes made.

Usage review: build copies five named `sce_sys` images only; `gen_assets.py`
embeds fonts, not `app/assets/brand/`. Inherited `app/assets/brand/*` and
`docs/media/*` remain tracked, unused by current app/package, not cleared for
publication. No unreviewed source assets deleted. This source folder is not
separately staged by build; deliver it with Corresponding Source.
