#!/usr/bin/env python3
"""Harissa64 V2 - builds the front end's glyph atlas (platform/xbox360/xb_ui_atlas.h).

The menus draw text and the pepper logo as textured quads from one 8-bit
signed distance field (SDF) atlas: each texel holds 0.5 + distance / (2 *
spread) to the nearest outline (atlas pixels, clamped), so one atlas gives
smooth anti-aliased text at any size, and soft shadows by sampling it with a
wider edge.

Glyphs: Inter Regular and Inter SemiBold (third_party/inter, SIL OFL 1.1):
ASCII 32-126, a few signs (EXTRAS), accented Latin letters and Cyrillic (the
menu languages); Arabic from Noto Sans Arabic Regular and SemiBold
(third_party/noto-arabic, SIL OFL 1.1): the presentation forms (letters already
in their joined shape: the translations are shaped by gen_lang.py) and Arabic
punctuation. The glyphs are listed by Unicode code point (UTF-8 text). The
first ones (ASCII and EXTRAS of both fonts) are packed first, at the places
they had before the other scripts were added.
Logo: a chili pepper drawn here from discs swept along Bezier curves (body,
highlight, stem and calyx), one SDF layer each, tinted at run time.

Needs Pillow (with FreeType) and NumPy. Run from anywhere:
    python platform/xbox360/tools/gen_ui_atlas.py [--png preview.png]
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
FONTS = [os.path.join(ROOT, "third_party", "inter", "Inter-Regular.ttf"),
         os.path.join(ROOT, "third_party", "inter", "Inter-SemiBold.ttf")]
ARABIC_FONTS = [os.path.join(ROOT, "third_party", "noto-arabic", "NotoSansArabic-Regular.ttf"),
                os.path.join(ROOT, "third_party", "noto-arabic", "NotoSansArabic-SemiBold.ttf")]
OUT = os.path.join(ROOT, "platform", "xbox360", "xb_ui_atlas.h")

BASE = 40          # em size of the atlas glyphs (atlas pixels)
SPREAD = 5         # distance range on each side of the outline (atlas pixels)
SS = 4             # supersampling of the binary shapes the distances come from
ATLAS_W, ATLAS_MAX_H = 1024, 4096   # the height is what the glyphs need, rounded up to a power of two
# Signs the UI uses (packed right after ASCII, as before): single angle quotes, ellipsis, bullet, middle dot.
EXTRAS = [0x2039, 0x203A, 0x2026, 0x2022, 0x00B7]
# Accented Latin letters (Spanish, German, French, Portuguese), Latin signs, Cyrillic (Russian).
LATIN = [0xA1, 0xAA, 0xAB, 0xB0, 0xBA, 0xBB, 0xBF] + list(range(0xC0, 0x100)) + [0x152, 0x153, 0x178,
         0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x20AC]
CYRILLIC = [0x401] + list(range(0x410, 0x450)) + [0x451, 0x2116]
# Arabic: comma, semicolon, question mark, and the presentation forms B (FE80-FEFC).
# They are drawn larger than the font's em, so that an alef is as tall as a
# Latin capital (Noto's Arabic looks small next to Inter otherwise).
ARABIC = [0x060C, 0x061B, 0x061F] + list(range(0xFE80, 0xFEFD))
LOGO = 128         # logo layers: LOGO x LOGO atlas pixels for a 256-unit drawing
LOGO_SPREAD = 10


def edt_sq(feature):
    """Squared Euclidean distance to the nearest True pixel (exact, two passes)."""
    h, w = feature.shape
    inf = float(h * h + w * w)
    f = np.where(feature, 0.0, inf)
    ys = np.arange(h, dtype=np.float64)
    # Columns: d1[y, x] = min_y' f[y', x] + (y - y')^2
    d1 = np.full((h, w), inf)
    for y in range(h):
        d1[y] = np.min(f + ((ys - y) ** 2)[:, None], axis=0)
    xs = np.arange(w, dtype=np.float64)
    d2 = np.full((h, w), inf)
    for x in range(w):
        d2[:, x] = np.min(d1 + ((xs - x) ** 2)[None, :], axis=1)
    return d2


def sdf_from_mask(mask, ss, spread):
    """mask: supersampled binary image (True inside), size a multiple of ss.
    Returns the 8-bit SDF at 1/ss resolution."""
    inside = mask
    d_out = np.sqrt(edt_sq(inside))          # outside pixels: distance to the shape
    d_in = np.sqrt(edt_sq(~inside))          # inside pixels: distance to the outside
    sd = np.where(inside, -(d_in - 0.5), d_out - 0.5) / ss   # signed, in output pixels (negative inside)
    h, w = sd.shape
    sd = sd.reshape(h // ss, ss, w // ss, ss).mean(axis=(1, 3))
    v = 0.5 - sd / (2.0 * spread)
    return np.clip(np.round(v * 255.0), 0, 255).astype(np.uint8)


def glyph_sdf(font_hi, ch):
    """SDF of one glyph. Returns (bitmap or None, left, top, advance) in BASE units:
    left/top: position of the bitmap's top-left corner relative to the pen on the baseline."""
    adv = font_hi.getlength(ch) / SS
    box = font_hi.getbbox(ch, anchor="ls")
    if box[2] <= box[0] or box[3] <= box[1]:
        return None, 0.0, 0.0, adv
    pad = (SPREAD + 1) * SS
    # Align the drawing on the output grid: origin at a multiple of SS.
    x0 = (box[0] // SS) * SS - pad
    y0 = (box[1] // SS) * SS - pad
    x1 = -(-box[2] // SS) * SS + pad
    y1 = -(-box[3] // SS) * SS + pad
    img = Image.new("L", (x1 - x0, y1 - y0), 0)
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font_hi, fill=255, anchor="ls")
    mask = np.array(img) >= 128
    return sdf_from_mask(mask, SS, SPREAD), x0 / SS, y0 / SS, adv


# ---- The logo: a chili pepper, 256 x 256 drawing units ----
def bezier(p, t):
    p = [np.array(q, dtype=np.float64) for q in p]
    if len(p) == 3:
        return (1 - t) ** 2 * p[0] + 2 * (1 - t) * t * p[1] + t ** 2 * p[2]
    return (1 - t) ** 3 * p[0] + 3 * (1 - t) ** 2 * t * p[1] + 3 * (1 - t) * t ** 2 * p[2] + t ** 3 * p[3]


def sweep(draw, k, pts, radius, steps=400, t0=0.0, t1=1.0):
    for i in range(steps + 1):
        t = t0 + (t1 - t0) * i / steps
        c = bezier(pts, t)
        r = radius(t)
        if r <= 0:
            continue
        draw.ellipse([(c[0] - r) * k, (c[1] - r) * k, (c[0] + r) * k, (c[1] + r) * k], fill=255)


BODY = [(176, 78), (158, 150), (120, 196), (34, 222)]


def logo_layers():
    n = LOGO * SS
    k = n / 256.0
    layers = []
    # Body: discs along a gentle S curve, round shoulder, thinning to the tip.
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)
    sweep(d, k, BODY, lambda t: 36.0 * (1.0 - t) ** 0.8 * min(1.0, 0.75 + t * 2.5) + 3.0 * (1 - t) + 2.0)
    layers.append(img)
    # Highlight: a thin stroke on the upper-left side of the body.
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)
    hl = [(158, 96), (146, 140), (118, 176), (70, 198)]
    sweep(d, k, hl, lambda t: 6.0 * np.sin(np.pi * t) ** 0.7, t0=0.02, t1=0.98)
    layers.append(img)
    # Stem and calyx: three lobes over the shoulder and a curved stem.
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)
    # Calyx: a band across the shoulder, scalloped into three leaves.
    sweep(d, k, [(144, 86), (174, 54), (210, 84)], lambda t: 7.0 + 6.0 * abs(np.sin(np.pi * 1.5 * t + 0.4)) * np.sin(np.pi * t) ** 0.4)
    sweep(d, k, [(158, 74), (176, 64), (194, 74)], lambda t: 9.0)
    sweep(d, k, [(176, 64), (182, 34), (214, 18)], lambda t: 7.5 - 2.5 * t)
    layers.append(img)
    out = []
    for img in layers:
        mask = np.array(img) >= 128
        out.append(sdf_from_mask(mask, SS, LOGO_SPREAD))
    return out


def rle(data):
    """Bytes: c < 128: c + 1 literal bytes follow; c >= 128: the next byte, c - 126 times."""
    out = bytearray()
    i, n = 0, len(data)
    lit = bytearray()

    def flush():
        j = 0
        while j < len(lit):
            chunk = lit[j:j + 128]
            out.append(len(chunk) - 1)
            out.extend(chunk)
            j += 128
        lit.clear()

    while i < n:
        j = i
        while j < n and j - i < 129 and data[j] == data[i]:
            j += 1
        if j - i >= 3:
            flush()
            out.append(j - i + 126)
            out.append(data[i])
            i = j
        else:
            lit.append(data[i])
            i += 1
    flush()
    return bytes(out)


class Packer:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.x = self.y = self.row = 0

    def place(self, bw, bh):
        if self.x + bw > self.w:
            self.x = 0
            self.y += self.row + 1
            self.row = 0
        if self.y + bh > self.h:
            raise SystemExit("atlas full")
        p = (self.x, self.y)
        self.x += bw + 1
        self.row = max(self.row, bh)
        return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", help="also write the atlas as a PNG (preview)")
    args = ap.parse_args()

    atlas = np.zeros((ATLAS_MAX_H, ATLAS_W), dtype=np.uint8)
    pk = Packer(ATLAS_W, ATLAS_MAX_H)
    first = [c for c in range(32, 127)] + EXTRAS
    later = LATIN + CYRILLIC
    fonts = []
    # Logo layers first (largest).
    logos = []
    for sd in logo_layers():
        x, y = pk.place(sd.shape[1], sd.shape[0])
        atlas[y:y + sd.shape[0], x:x + sd.shape[1]] = sd
        logos.append((x, y, sd.shape[1], sd.shape[0]))

    def put(font_hi, code, glyphs):
        sd, left, top, adv = glyph_sdf(font_hi, chr(code))
        if sd is None:
            glyphs[code] = (0, 0, 0, 0, 0.0, 0.0, adv)
            return
        x, y = pk.place(sd.shape[1], sd.shape[0])
        atlas[y:y + sd.shape[0], x:x + sd.shape[1]] = sd
        glyphs[code] = (x, y, sd.shape[1], sd.shape[0], left, top, adv)

    his = []
    for path in FONTS:
        hi = ImageFont.truetype(path, BASE * SS)
        asc, desc = hi.getmetrics()
        cap = -hi.getbbox("H", anchor="ls")[1] / SS
        xh = -hi.getbbox("x", anchor="ls")[1] / SS
        glyphs = {}
        for code in first:
            put(hi, code, glyphs)
        fonts.append((os.path.basename(path), asc / SS, desc / SS, cap, xh, glyphs))
        his.append(hi)
        print("%s: ascent %.1f descent %.1f cap %.1f x %.1f" % (os.path.basename(path), asc / SS, desc / SS, cap, xh))
    # Then the other scripts, after the glyphs that were there first.
    for i in range(len(FONTS)):
        for code in later:
            put(his[i], code, fonts[i][5])
    for i, path in enumerate(ARABIC_FONTS):
        probe = ImageFont.truetype(path, BASE * SS)
        alef = -probe.getbbox(chr(0xFE8D), anchor="ls")[1] / SS
        scale = fonts[i][3] / alef
        ar = ImageFont.truetype(path, int(round(BASE * SS * scale)))
        print("%s: alef %.1f, drawn x%.3f" % (os.path.basename(path), alef, scale))
        for code in ARABIC:
            put(ar, code, fonts[i][5])
    used = pk.y + pk.row
    # A power of two: texture coordinates (y / height) stay exact, so the glyphs
    # that were there before the other scripts sample the same texels as then.
    atlas_h = 512
    while atlas_h < used + 1:
        atlas_h *= 2
    atlas = atlas[:atlas_h]
    print("atlas %dx%d, %d rows used" % (ATLAS_W, atlas_h, used))
    if args.png:
        Image.fromarray(atlas).save(args.png)
    codes = sorted(set(first + later + ARABIC))
    assert codes[:95] == list(range(32, 127))

    packed = rle(atlas.tobytes())
    print("RLE: %d -> %d bytes" % (ATLAS_W * atlas_h, len(packed)))
    lines = []
    lines.append("// Harissa64 V2 - front-end glyph atlas. GENERATED by platform/xbox360/tools/gen_ui_atlas.py: do not edit.")
    lines.append("// Signed distance field, 8 bits: 0.5 + distance / (2 * spread) (inside > 0.5), RLE-coded:")
    lines.append("// c < 128: c + 1 literal bytes follow; c >= 128: the next byte repeated c - 126 times.")
    lines.append("// Glyphs: Inter Regular and SemiBold (c) 2016 The Inter Project Authors, SIL Open Font License 1.1")
    lines.append("// (third_party/inter/OFL.txt); Arabic: Noto Sans Arabic Regular and SemiBold (c) 2022 The Noto Project Authors,")
    lines.append("// SIL Open Font License 1.1 (third_party/noto-arabic/OFL.txt). Logo: the Harissa pepper, drawn by the generator.")
    lines.append("#ifndef XB_UI_ATLAS_H")
    lines.append("#define XB_UI_ATLAS_H")
    lines.append("")
    lines.append("#define UI_ATLAS_W %d" % ATLAS_W)
    lines.append("#define UI_ATLAS_H %d" % atlas_h)
    lines.append("#define UI_ATLAS_BASE %d.0f      // em size of the glyphs, atlas pixels" % BASE)
    lines.append("#define UI_ATLAS_SPREAD %d.0f" % SPREAD)
    lines.append("#define UI_ATLAS_LOGO_SPREAD %d.0f" % LOGO_SPREAD)
    lines.append("#define UI_ATLAS_COUNT %d      // glyphs per font, by code point (s_atlasCode); the first 95 are ASCII 32..126" % len(codes))
    lines.append("")
    lines.append("// One glyph: atlas rectangle, its top-left relative to the pen on the baseline, advance (em units of UI_ATLAS_BASE).")
    lines.append("struct UiAtlasGlyph { unsigned short x, y, w, h; float left, top, advance; };")
    lines.append("struct UiAtlasFont { float ascent, descent, cap, xHeight; };")
    lines.append("struct UiAtlasRect { unsigned short x, y, w, h; };")
    lines.append("")
    lines.append("static const UiAtlasFont s_atlasFont[%d] = {" % len(fonts))
    for name, asc, desc, cap, xh, _ in fonts:
        lines.append("    { %.3ff, %.3ff, %.3ff, %.3ff },   // %s" % (asc, desc, cap, xh, name))
    lines.append("};")
    lines.append("")
    lines.append("// The Unicode code point of each glyph, in increasing order.")
    lines.append("static const unsigned short s_atlasCode[UI_ATLAS_COUNT] = {")
    for i in range(0, len(codes), 16):
        lines.append("    " + ", ".join("0x%04X" % c for c in codes[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("static const UiAtlasGlyph s_atlasGlyph[%d][UI_ATLAS_COUNT] = {" % len(fonts))
    for name, _, _, _, _, glyphs in fonts:
        lines.append("  {   // %s" % name)
        for code in codes:
            g = glyphs.get(code, (0, 0, 0, 0, 0.0, 0.0, 0.0))
            lines.append("    { %d, %d, %d, %d, %.3ff, %.3ff, %.3ff },   // U+%04X" % (g + (code,)))
        lines.append("  },")
    lines.append("};")
    lines.append("")
    lines.append("// Logo layers (a 256 x 256 drawing): body, highlight, stem and calyx.")
    lines.append("static const UiAtlasRect s_atlasLogo[%d] = {" % len(logos))
    for r in logos:
        lines.append("    { %d, %d, %d, %d }," % r)
    lines.append("};")
    lines.append("")
    lines.append("static const unsigned char s_atlasRle[%d] = {" % len(packed))
    for i in range(0, len(packed), 32):
        lines.append("    " + ",".join(str(b) for b in packed[i:i + 32]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
