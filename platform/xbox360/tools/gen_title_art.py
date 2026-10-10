#!/usr/bin/env python3
"""Harissa64 V2 - draws the title art (platform/xbox360/title/art/).

Original art, no third-party images: the Harissa pepper of the front end
(same Bezier geometry and colours as gen_ui_atlas.py / UiLogo), the
"Harissa64" wordmark in Inter SemiBold (third_party/inter, SIL OFL 1.1),
on the UI's dark background with its red-orange accent.

Outputs:
  cover.png        900 x 1270  front cover (Xbox 360 box proportions)
  cover_small.png  219 x 300   the same for dashboards (Aurora's box size)
  background.png   1280 x 720  dashboard background
  icon.png         64 x 64     title image (embedded in the XEX's SPA, X_IMAGEID_GAME)

Needs Pillow (with FreeType) and NumPy:
    python platform/xbox360/tools/gen_title_art.py
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_ui_atlas as atlas  # noqa: E402  (pepper geometry)

ROOT = atlas.ROOT
OUT = os.path.join(ROOT, "platform", "xbox360", "title", "art")
FONT_SEMI = os.path.join(ROOT, "third_party", "inter", "Inter-SemiBold.ttf")
FONT_REG = os.path.join(ROOT, "third_party", "inter", "Inter-Regular.ttf")

BG = (18, 19, 26)
BG2 = (28, 30, 40)
ACCENT = (234, 76, 58)
TEXT = (240, 240, 245)
DIM = (150, 154, 170)

# Layer colours of UiLogo (xb_ui.cpp): top and bottom of the 256-unit drawing, alpha 0..255.
PEPPER = [((255, 98, 72, 255), (190, 30, 26, 255)),       # body
          ((255, 236, 228, 110), (255, 236, 228, 70)),    # highlight
          ((120, 206, 92, 255), (46, 142, 64, 255))]      # stem and calyx


def pepper_masks(size):
    """The three pepper layers as anti-aliased masks of size x size (float 0..1)."""
    ss = 4
    n = size * ss
    k = n / 256.0
    out = []
    shapes = []
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)
    atlas.sweep(d, k, atlas.BODY, lambda t: 36.0 * (1.0 - t) ** 0.8 * min(1.0, 0.75 + t * 2.5) + 3.0 * (1 - t) + 2.0)
    shapes.append(img)
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)
    atlas.sweep(d, k, [(158, 96), (146, 140), (118, 176), (70, 198)], lambda t: 6.0 * np.sin(np.pi * t) ** 0.7, t0=0.02, t1=0.98)
    shapes.append(img)
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)
    atlas.sweep(d, k, [(144, 86), (174, 54), (210, 84)], lambda t: 7.0 + 6.0 * abs(np.sin(np.pi * 1.5 * t + 0.4)) * np.sin(np.pi * t) ** 0.4)
    atlas.sweep(d, k, [(158, 74), (176, 64), (194, 74)], lambda t: 9.0)
    atlas.sweep(d, k, [(176, 64), (182, 34), (214, 18)], lambda t: 7.5 - 2.5 * t)
    shapes.append(img)
    for img in shapes:
        out.append(np.asarray(img.resize((size, size), Image.LANCZOS), dtype=np.float64) / 255.0)
    return out


def comp(dst, rgb, alpha, x, y):
    """Alpha-blends an RGB float image (h, w, 3) with alpha (h, w) into dst (float) at x, y (clipped)."""
    h, w = alpha.shape
    H, W = dst.shape[:2]
    x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, W), min(y + h, H)
    if x0 >= x1 or y0 >= y1:
        return
    a = alpha[y0 - y:y1 - y, x0 - x:x1 - x][..., None]
    c = rgb[y0 - y:y1 - y, x0 - x:x1 - x]
    dst[y0:y1, x0:x1] = dst[y0:y1, x0:x1] * (1 - a) + c * a


def pepper_box():
    """Bounding box of the pepper in drawing units (0..256): x0, y0, x1, y1."""
    m = sum(pepper_masks(256)) > 0.05
    ys, xs = np.nonzero(m)
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def place_pepper(dst, cx, cy, extent, shadow=True):
    """Draws the pepper with its bounding box's larger side = extent, centred on cx, cy."""
    x0, y0, x1, y1 = pepper_box()
    size = int(round(extent * 256.0 / max(x1 - x0, y1 - y0)))
    k = size / 256.0
    draw_pepper(dst, int(round(cx - (x0 + x1) / 2 * k)), int(round(cy - (y0 + y1) / 2 * k)), size, shadow)


def draw_pepper(dst, x, y, size, shadow=True):
    masks = pepper_masks(size)
    t = np.linspace(0.0, 1.0, size)[:, None, None]
    if shadow:
        sh = Image.fromarray((masks[0] * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(size * 0.035))
        sa = np.asarray(sh, dtype=np.float64) / 255.0 * 0.55
        comp(dst, np.zeros((size, size, 3)), sa, x, y + int(size * 0.045))
    for m, (top, bot) in zip(masks, PEPPER):
        top = np.array(top, dtype=np.float64)
        bot = np.array(bot, dtype=np.float64)
        col = top[None, None, :] * (1 - t) + bot[None, None, :] * t       # (size, 1, 4)
        rgb = np.broadcast_to(col[..., :3], (size, size, 3))
        a = m * (col[..., 3] / 255.0)
        comp(dst, rgb, a, x, y)


def background(w, h, glow_xy, glow_r, glow_amount=0.30):
    """Dark vertical gradient, a warm glow of the accent and a faint diagonal pattern."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    t = yy / max(h - 1, 1)
    base = np.array(BG2)[None, None, :] * (1 - t[..., None]) + np.array(BG)[None, None, :] * t[..., None]
    d = np.hypot(xx - glow_xy[0], yy - glow_xy[1]) / glow_r
    g = np.exp(-d * d * 1.6)[..., None] * glow_amount
    img = base * (1 - g) + np.array(ACCENT)[None, None, :] * g
    # Thin diagonal stripes, very faint, fading out away from the glow.
    stripe = ((xx + yy) % 28 < 1.4).astype(np.float64) * 0.035 * np.exp(-d * 0.8)
    img = img + stripe[..., None] * 255
    # Vignette.
    vx = (xx / w - 0.5) * 2
    vy = (yy / h - 0.5) * 2
    v = 1 - 0.28 * np.clip(vx * vx * 0.6 + vy * vy * 0.4, 0, 1)
    return img * v[..., None]


def text_layer(w, h, items):
    """items: (x, y, text, font, colour rgb, anchor, letter spacing). Returns (rgb, alpha) float images."""
    rgb = np.zeros((h, w, 3))
    alpha = np.zeros((h, w))
    for x, y, s, font, col, anchor, spacing in items:
        img = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(img)
        if spacing:
            total = sum(font.getlength(c) for c in s) + spacing * (len(s) - 1)
            px = x - total / 2 if anchor[0] == "m" else x
            for c in s:
                d.text((px, y), c, font=font, fill=255, anchor="l" + anchor[1])
                px += font.getlength(c) + spacing
        else:
            d.text((x, y), s, font=font, fill=255, anchor=anchor)
        a = np.asarray(img, dtype=np.float64) / 255.0
        rgb = rgb * (1 - a[..., None]) + np.array(col)[None, None, :] * a[..., None]
        alpha = np.maximum(alpha, a)
    return rgb, alpha


def wordmark(dst, cx, baseline, size, v2=True, max_w=None):
    """'Harissa64' with '64' in the accent, optionally followed by a 'V2' pill; centred on cx.
    The size shrinks until the whole mark fits in max_w."""
    while max_w:
        f = ImageFont.truetype(FONT_SEMI, size)
        fv = ImageFont.truetype(FONT_SEMI, int(size * 0.36))
        tw = f.getlength("Harissa64") + (size * 0.22 + fv.getlength("V2") + size * 0.42 if v2 else 0)
        if tw <= max_w:
            break
        size -= 2
    f = ImageFont.truetype(FONT_SEMI, size)
    fv = ImageFont.truetype(FONT_SEMI, int(size * 0.36))
    w1 = f.getlength("Harissa")
    w2 = f.getlength("64")
    pill_w = fv.getlength("V2") + size * 0.42
    pill_h = size * 0.46
    gap = size * 0.22
    total = w1 + w2 + (gap + pill_w if v2 else 0)
    x = cx - total / 2
    H, W = dst.shape[:2]
    # Soft shadow under the text.
    rgb, a = text_layer(W, H, [(x, baseline, "Harissa", f, TEXT, "ls", 0), (x + w1, baseline, "64", f, ACCENT, "ls", 0)])
    sh = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(size * 0.06)), dtype=np.float64) / 255.0
    comp(dst, np.zeros((H, W, 3)), np.roll(sh, int(size * 0.04), axis=0) * 0.6, 0, 0)
    comp(dst, rgb, a, 0, 0)
    if v2:
        cap = -f.getbbox("H", anchor="ls")[1]
        px = x + w1 + w2 + gap
        py = baseline - cap / 2 - pill_h / 2
        ss = 4
        pw, ph = int(pill_w * ss), int(pill_h * ss)
        m = Image.new("L", (pw, ph), 0)
        ImageDraw.Draw(m).rounded_rectangle([0, 0, pw - 1, ph - 1], radius=ph * 0.32, fill=255)
        m = np.asarray(m.resize((int(pill_w), int(pill_h)), Image.LANCZOS), dtype=np.float64) / 255.0
        hh, ww = m.shape
        t = np.linspace(0, 1, hh)[:, None, None]
        col = np.array((246, 104, 84))[None, None, :] * (1 - t) + np.array(ACCENT)[None, None, :] * t
        comp(dst, np.broadcast_to(col, (hh, ww, 3)), m, int(px), int(py))
        rgb, a = text_layer(W, H, [(px + ww / 2, py + hh / 2, "V2", fv, TEXT, "mm", 0)])
        comp(dst, rgb, a, 0, 0)


def save(arr, name, size=None):
    img = Image.fromarray(np.clip(np.round(arr), 0, 255).astype(np.uint8), "RGB")
    if size:
        img = img.resize(size, Image.LANCZOS)
    path = os.path.join(OUT, name)
    img.save(path, optimize=True)
    print("wrote", os.path.relpath(path, ROOT), img.size)
    return img


def cover():
    W, H = 900, 1270
    img = background(W, H, (W / 2, H * 0.40), W * 0.62)
    # Accent bar at the top and a thin rule at the bottom.
    img[0:14, :] = ACCENT
    place_pepper(img, W / 2, H * 0.40, 600)
    wordmark(img, W / 2, 1000, 150, max_w=W * 0.84)
    f_sub = ImageFont.truetype(FONT_REG, 38)
    f_small = ImageFont.truetype(FONT_SEMI, 24)
    rgb, a = text_layer(W, H, [
        (W / 2, 1072, "N64 emulator for Xbox 360", f_sub, DIM, "ms", 0),
        (W / 2, 1222, "HOMEBREW  ·  OPEN SOURCE", f_small, (110, 114, 130), "ms", 3),
    ])
    comp(img, rgb, a, 0, 0)
    img[1160:1162, 300:600] = img[1160:1162, 300:600] * 0.4 + np.array(ACCENT) * 0.6
    save(img, "cover.png")
    save(img, "cover_small.png", (219, 300))


def bg_wide():
    W, H = 1280, 720
    img = background(W, H, (W * 0.70, H * 0.46), W * 0.42, 0.26)
    place_pepper(img, W * 0.70, H * 0.48, 470)
    save(img, "background.png")


def icon():
    # Drawn large and reduced: dark rounded square, the pepper filling it.
    S = 512
    img = background(S, S, (S / 2, S * 0.46), S * 0.55, 0.34)
    place_pepper(img, S / 2, S / 2, S * 0.86)
    save(img, "icon.png", (64, 64))


def main():
    os.makedirs(OUT, exist_ok=True)
    cover()
    bg_wide()
    icon()


if __name__ == "__main__":
    main()
