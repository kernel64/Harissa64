#!/usr/bin/env python3
"""Harissa64 V2 - draws the flags of the language list (platform/xbox360/xb_flags.h).

Each flag is drawn here from its official geometry (no image files), 4 times
larger, then reduced to FLAG_W x FLAG_H (3:2) and stored as a PNG in the
header (the console decodes it with h64_png_decode). Order: the languages of
gen_lang.py (en, es, de, fr, ar, pt, ru):
  en  the United Kingdom and the United States, split along a diagonal
  es  Spain (red, yellow, red; a simplified coat of arms)
  de  Germany
  fr  France
  ar  Tunisia
  pt  Brazil
  ru  Russia

Needs Pillow. Run from anywhere:
    python platform/xbox360/tools/gen_flags.py [--png DIR]
"""
import argparse
import io
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "platform", "xbox360", "xb_flags.h")
FLAG_W, FLAG_H = 192, 128
SS = 4
W, H = FLAG_W * SS, FLAG_H * SS


def new():
    img = Image.new("RGB", (W, H), (0, 0, 0))
    return img, ImageDraw.Draw(img)


def star(cx, cy, r, angle=-90.0, inner=0.381966):
    """A five-pointed star's polygon: first point at `angle` degrees (screen: -90 points up)."""
    pts = []
    for i in range(10):
        a = math.radians(angle + i * 36)
        rr = r if i % 2 == 0 else r * inner
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    return pts


def stripes(colors, vertical):
    img, d = new()
    n = len(colors)
    for i, c in enumerate(colors):
        if vertical:
            d.rectangle([W * i / n, 0, W * (i + 1) / n, H], fill=c)
        else:
            d.rectangle([0, H * i / n, W, H * (i + 1) / n], fill=c)
    return img


def union_jack():
    """The Union Jack over the whole W x H field (its parts in units of a 30-high flag)."""
    import numpy as np
    blue, red, white = (1, 33, 105), (200, 16, 46), (255, 255, 255)
    u = H / 30.0
    y, x = np.mgrid[0:H, 0:W].astype(np.float64) + 0.5
    norm = math.hypot(W, H)
    d1 = (x * H - y * W) / norm              # > 0 above the top-left to bottom-right diagonal
    d2 = (x * H + y * W - W * H) / norm      # > 0 below the bottom-left to top-right diagonal
    left, top = x < W / 2, y < H / 2
    img = np.zeros((H, W, 3), dtype=np.uint8)
    img[:] = blue
    img[(abs(d1) < 3 * u) | (abs(d2) < 3 * u)] = white
    # The red saltire, counterchanged: in each arm it lies on the side the arm turns to clockwise.
    r1 = np.where(left, (d1 > -2 * u) & (d1 < 0), (d1 < 2 * u) & (d1 > 0)) & (top == left)
    r2 = np.where(left, (d2 < 2 * u) & (d2 > 0), (d2 > -2 * u) & (d2 < 0)) & (top != left)
    img[r1 | r2] = red
    img[(abs(x - W / 2) < 5 * u) | (abs(y - H / 2) < 5 * u)] = white
    img[(abs(x - W / 2) < 3 * u) | (abs(y - H / 2) < 3 * u)] = red
    return Image.fromarray(img)


def us_flag():
    img, d = new()
    red, white, blue = (178, 34, 52), (255, 255, 255), (60, 59, 110)
    for i in range(13):
        d.rectangle([0, H * i / 13.0, W, H * (i + 1) / 13.0], fill=red if i % 2 == 0 else white)
    cw, ch = 0.76 * H, H * 7 / 13.0
    d.rectangle([0, 0, cw, ch], fill=blue)
    r = 0.0616 * H / 2
    for row in range(9):
        cols = 6 if row % 2 == 0 else 5
        for j in range(cols):
            cx = (2 * j + 1 if row % 2 == 0 else 2 * j + 2) * cw / 12.0
            cy = (row + 1) * ch / 10.0
            d.polygon(star(cx, cy, r), fill=white)
    return img


def english():
    uk, us = union_jack(), us_flag()
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).polygon([(0, 0), (W, 0), (W, H)], fill=255)   # the United States above the diagonal
    img = Image.composite(us, uk, mask)
    ImageDraw.Draw(img).line([(0, 0), (W, H)], fill=(255, 255, 255), width=int(2.5 * SS))
    return img


def spain():
    red, yellow = (170, 21, 27), (241, 191, 0)
    img, d = new()
    d.rectangle([0, 0, W, H], fill=red)
    d.rectangle([0, H / 4.0, W, H * 3 / 4.0], fill=yellow)
    # A simplified coat of arms on the hoist side (its axis at half the height from the hoist).
    cx, cy, k = H / 2.0, H / 2.0, H / 100.0
    dark, grey, purple, white = (60, 40, 10), (200, 200, 205), (120, 30, 110), (240, 240, 240)
    # Pillars of Hercules, with their red scrolls and crowns.
    for px in (cx - 19 * k, cx + 19 * k):
        d.rectangle([px - 3 * k, cy - 12 * k, px + 3 * k, cy + 15 * k], fill=grey, outline=dark, width=SS)
        d.rectangle([px - 4.5 * k, cy + 14 * k, px + 4.5 * k, cy + 17 * k], fill=grey, outline=dark, width=SS)
        d.rectangle([px - 4.5 * k, cy - 14 * k, px + 4.5 * k, cy - 11 * k], fill=yellow, outline=dark, width=SS)
        d.polygon([(px - 6 * k, cy - 2 * k), (px + 6 * k, cy - 5 * k), (px + 6 * k, cy - 1 * k), (px - 6 * k, cy + 2 * k)], fill=red, outline=dark)
        d.ellipse([px - 3.5 * k, cy - 19 * k, px + 3.5 * k, cy - 13.5 * k], fill=yellow, outline=dark, width=SS)
    # The shield: four quarters and Granada at the point.
    l, r_, t, b = cx - 12 * k, cx + 12 * k, cy - 10 * k, cy + 16 * k
    mid_x, mid_y = cx, cy + 2 * k
    d.rectangle([l, t, mid_x, mid_y], fill=red)                       # Castile
    d.rectangle([cx - 8 * k, t + 3 * k, cx - 4 * k, mid_y - 3 * k], fill=yellow)
    d.rectangle([mid_x, t, r_, mid_y], fill=white)                    # Leon
    d.ellipse([cx + 3 * k, t + 3 * k, cx + 9 * k, mid_y - 3 * k], fill=purple)
    d.rectangle([l, mid_y, mid_x, b - 5 * k], fill=yellow)            # Aragon
    for i in range(4):
        x0 = l + (i + 0.5) * 3 * k
        d.rectangle([x0, mid_y, x0 + 1.5 * k, b - 5 * k], fill=red)
    d.rectangle([mid_x, mid_y, r_, b - 5 * k], fill=red)              # Navarre
    d.rectangle([cx + 3 * k, mid_y + 2 * k, cx + 9 * k, b - 7 * k], outline=yellow, width=SS)
    d.chord([l, b - 12 * k, r_, b + 2 * k], 0, 180, fill=white)       # Granada
    d.ellipse([cx - 2 * k, b - 6 * k, cx + 2 * k, b - 1 * k], fill=(200, 60, 60))
    d.ellipse([cx - 3 * k, cy - 1 * k, cx + 3 * k, cy + 5 * k], fill=(40, 70, 170), outline=red, width=SS)
    d.line([(l, t), (l, b - 5 * k)], fill=dark, width=SS)
    d.line([(r_, t), (r_, b - 5 * k)], fill=dark, width=SS)
    d.arc([l, b - 12 * k, r_, b + 2 * k], 0, 180, fill=dark, width=SS)
    d.line([(l, t), (r_, t)], fill=dark, width=SS)
    # The royal crown over it.
    d.rectangle([cx - 11 * k, t - 4 * k, cx + 11 * k, t - 1 * k], fill=yellow, outline=dark, width=SS)
    d.chord([cx - 11 * k, t - 16 * k, cx + 11 * k, t + 2 * k], 180, 360, fill=red, outline=dark)
    for i in range(5):
        x0 = cx - 10 * k + i * 5 * k
        d.ellipse([x0 - 1.4 * k, t - 9 * k, x0 + 1.4 * k, t - 6.2 * k], fill=yellow)
    d.ellipse([cx - 2 * k, t - 19 * k, cx + 2 * k, t - 15 * k], fill=yellow, outline=dark, width=SS)
    return img


def tunisia():
    """From the flag's construction: a 120 x 80 field, white disc r 20, crescent from
    circles r 15 (centre) and r 12 (4 units towards the fly), star with a point to the hoist."""
    red, white = (231, 0, 19), (255, 255, 255)
    img, d = new()
    k = H / 80.0
    cx, cy = W / 2.0, H / 2.0

    def circle(x, y, r, c):
        d.ellipse([cx + (x - r) * k, cy + (y - r) * k, cx + (x + r) * k, cy + (y + r) * k], fill=c)

    d.rectangle([0, 0, W, H], fill=red)
    circle(0, 0, 20, white)
    circle(0, 0, 15, red)
    circle(4, 0, 12, white)
    pts = [(-5, 0), (11.281, -5.29), (1.22, 8.56), (1.22, -8.56), (11.28, 5.29)]
    sx = sum(p[0] for p in pts) / 5.0
    rad = math.hypot(-5 - sx, 0)
    d.polygon([(cx + x * k, cy + y * k) for x, y in star(sx, 0, rad, angle=180.0)], fill=red)
    return img


def brazil():
    """The flag's own construction (4200 x 2940 units around the centre), fitted to the
    height; the green field fills the wider card."""
    green, yellow, blue, white = (0, 148, 64), (255, 203, 0), (48, 38, 129), (255, 255, 255)
    img, d = new()
    k = H / 2940.0
    cx, cy = W / 2.0, H / 2.0

    def p(x, y):
        return (cx + x * k, cy + y * k)

    d.rectangle([0, 0, W, H], fill=green)
    d.polygon([p(-1743, 0), p(0, 1113), p(1743, 0), p(0, -1113)], fill=yellow)
    d.ellipse([p(-735, -735), p(735, 735)], fill=blue)
    # The white band: between arcs of radius 1680 and 1785 around (-420, 1470), inside the globe.
    band = Image.new("L", (W, H), 0)
    bd = ImageDraw.Draw(band)
    bd.ellipse([p(-420 - 1785, 1470 - 1785), p(-420 + 1785, 1470 + 1785)], fill=255)
    bd.ellipse([p(-420 - 1680, 1470 - 1680), p(-420 + 1680, 1470 + 1680)], fill=0)
    globe = Image.new("L", (W, H), 0)
    ImageDraw.Draw(globe).ellipse([p(-735, -735), p(735, 735)], fill=255)
    band = Image.composite(band, Image.new("L", (W, H), 0), globe)
    img.paste(Image.new("RGB", (W, H), white), (0, 0), band)
    d = ImageDraw.Draw(img)
    stars = [(-600, -132, 31.5), (-535, 177, 31.5), (-625, 243, 26.25), (-463, 132, 15), (-382, 250, 26.25), (-404, 323, 21),
             (228, -228, 31.5), (515, 258, 31.5), (617, 265, 21), (545, 323, 26.25), (368, 477, 26.25), (367, 551, 21),
             (441, 419, 21), (500, 382, 26.25), (365, 405, 21), (-280, 30, 26.25), (200, -37, 21), (0, 330, 31.5),
             (85, 184, 26.25), (0, 118, 26.25), (-74, 184, 21), (-37, 235, 15), (220, 495, 26.25), (283, 430, 21),
             (162, 412, 21), (-295, 390, 31.5), (0, 575, 10.5)]
    for x, y, s in stars:
        # A star of radius s, drawn at least a little over one pixel of the final flag.
        r = max(s * k * 1.15, 0.9 * SS)
        d.polygon(star(cx + x * k, cy + y * k, r), fill=white)
    return img


def make_all():
    return [
        ("en", english()),
        ("es", spain()),
        ("de", stripes([(0, 0, 0), (221, 0, 0), (255, 206, 0)], False)),
        ("fr", stripes([(0, 85, 164), (255, 255, 255), (239, 65, 53)], True)),
        ("ar", tunisia()),
        ("pt", brazil()),
        ("ru", stripes([(255, 255, 255), (0, 57, 166), (213, 43, 30)], False)),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", help="also write each flag as DIR/<code>.png (preview)")
    args = ap.parse_args()
    lines = []
    lines.append("// Harissa64 V2 - the flags of the language list. GENERATED by platform/xbox360/tools/gen_flags.py: do not edit.")
    lines.append("// Drawn by the generator from each flag's geometry; PNG files (decoded with h64_png_decode), in the order of LANG_*.")
    lines.append("#ifndef XB_FLAGS_H")
    lines.append("#define XB_FLAGS_H")
    lines.append("")
    lines.append("#define FLAG_W %d" % FLAG_W)
    lines.append("#define FLAG_H %d" % FLAG_H)
    lines.append("")
    names = []
    for code, img in make_all():
        small = img.resize((FLAG_W, FLAG_H), Image.LANCZOS)
        if args.png:
            os.makedirs(args.png, exist_ok=True)
            small.save(os.path.join(args.png, code + ".png"))
        buf = io.BytesIO()
        small.save(buf, "PNG", optimize=True)
        data = buf.getvalue()
        names.append("s_flag_%s" % code)
        lines.append("static const unsigned char s_flag_%s[%d] = {" % (code, len(data)))
        for i in range(0, len(data), 32):
            lines.append("    " + ",".join(str(b) for b in data[i:i + 32]) + ",")
        lines.append("};")
        print("%s: %d bytes" % (code, len(data)))
    lines.append("")
    lines.append("static const unsigned char *const s_flagPng[] = { %s };" % ", ".join(names))
    lines.append("static const unsigned int s_flagPngSize[] = { %s };" % ", ".join("sizeof(%s)" % n for n in names))
    lines.append("")
    lines.append("#endif")
    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
