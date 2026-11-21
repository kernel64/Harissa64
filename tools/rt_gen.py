#!/usr/bin/env python3
"""Synthetic render test scenes (phase 3, docs-local/render_tests_study.md).

Writes OUT/<case>/scene.rdp (platform/host/h64_rtdump.h) and case.json for each
case, built from RDP commands (layouts as ParaLLEl-RDP's rdp_command_builder.cpp,
MIT, and n64brew). Rectangles only for now: texture filters at fractional
positions, texture formats, combiner (1 and 2 cycles), blender, alpha compare,
render to texture. Every case is a fixed scene or a seeded random one (the
same seed always gives the same scene).

  python tools/rt_gen.py OUT [--filter TEXT] [--random N] [--seed S] [--score]

--score replays every case with the software RDP and the oracles and compares
(tools/rt_compare.py), then sums up (tools/rt_report.py). The same folder goes
to the console with tools/rt_console.py prepare/collect (OUT as the corpus).

Colour image: 320x240 RGBA 16-bit at 0x100000; textures from 0x400000; render
target for the two-pass case at 0x300000. Dither off unless a case says so.
"""
import argparse
import json
import os
import random
import struct

COLOR, TEXBASE, RTT = 0x100000, 0x400000, 0x300000
W, H = 320, 240

# Combiner inputs (n64brew "Color Combiner")
A_COMB, A_TEX0, A_TEX1, A_PRIM, A_SHADE, A_ENV, A_ONE, A_NOISE, A_ZERO = 0, 1, 2, 3, 4, 5, 6, 7, 15
B_COMB, B_TEX0, B_TEX1, B_PRIM, B_SHADE, B_ENV, B_ZERO = 0, 1, 2, 3, 4, 5, 15
C_COMB, C_TEX0, C_TEX1, C_PRIM, C_SHADE, C_ENV, C_TEX0A, C_PRIMA, C_ENVA, C_ZERO = 0, 1, 2, 3, 4, 5, 8, 10, 12, 31
D_COMB, D_TEX0, D_TEX1, D_PRIM, D_SHADE, D_ENV, D_ONE, D_ZERO = 0, 1, 2, 3, 4, 5, 6, 7
# alpha: a/b/d 0 comb 1 tex0 2 tex1 3 prim 4 shade 5 env 6 one 7 zero; c 0 lodfrac 1 tex0 2 tex1 3 prim 4 shade 5 env 6 primlod 7 zero
AA_COMB, AA_TEX0, AA_TEX1, AA_PRIM, AA_SHADE, AA_ENV, AA_ONE, AA_ZERO = 0, 1, 2, 3, 4, 5, 6, 7

FMT_RGBA, FMT_YUV, FMT_CI, FMT_IA, FMT_I = 0, 1, 2, 3, 4
SZ_4, SZ_8, SZ_16, SZ_32 = 0, 1, 2, 3

FILT_POINT, FILT_BILERP, FILT_AVERAGE = 0, 2, 3


class Scene:
    def __init__(self):
        self.cmds = []          # (hi, lo) pairs per command
        self.ram = {}           # addr -> bytes (N64 order)

    def put(self, *words):
        self.cmds.append(list(words))

    # ---- state
    def color_image(self, addr=COLOR, width=W, fmt=FMT_RGBA, size=SZ_16):
        self.put(0x3F000000 | fmt << 21 | size << 19 | (width - 1), addr)

    def scissor(self, x0=0, y0=0, x1=W, y1=H):
        self.put(0x2D000000 | (x0 * 4) << 12 | (y0 * 4), (x1 * 4) << 12 | (y1 * 4))

    def other_modes(self, cycle=0, filt=FILT_POINT, tlut=0, tlut_ia=0, persp=0, blend=0, rgb_dither=3, alpha_dither=3,
                    force_blend=0, alpha_compare=0, cvg_times_alpha=0, alpha_cvg_select=0, image_read=0,
                    cvg_dest=0, aa=0, color_on_cvg=0, z=0):
        hi = (cycle << 20 | persp << 19 | tlut << 15 | tlut_ia << 14 | filt << 12 | 6 << 9 |
              rgb_dither << 6 | alpha_dither << 4)
        lo = (blend << 16 | force_blend << 14 | alpha_cvg_select << 13 | cvg_times_alpha << 12 | cvg_dest << 8 |
              color_on_cvg << 7 | image_read << 6 | aa << 3 | alpha_compare)
        self.put(0x2F000000 | hi, lo)

    def combine(self, c0, a0, c1=None, a1=None):
        """c = (a, b, c, d) RGB inputs, a = (a, b, c, d) alpha inputs; second cycle defaults to the first."""
        c1 = c1 or c0
        a1 = a1 or a0
        w0 = 0x3C000000 | c0[0] << 20 | c0[2] << 15 | a0[0] << 12 | a0[2] << 9 | c1[0] << 5 | c1[2]
        w1 = (c0[1] << 28 | c1[1] << 24 | a1[0] << 21 | a1[2] << 18 | c0[3] << 15 | a0[1] << 12 | a0[3] << 9 |
              c1[3] << 6 | a1[1] << 3 | a1[3])
        self.put(w0, w1 & 0xFFFFFFFF)

    def fill_color(self, c):
        self.put(0x37000000, c)

    def prim(self, rgba, minlod=0, frac=0):
        self.put(0x3A000000 | minlod << 8 | frac, rgba)

    def env(self, rgba):
        self.put(0x3B000000, rgba)

    def blend_color(self, rgba):
        self.put(0x39000000, rgba)

    def sync(self, op=0x27):
        self.put(op << 24, 0)

    # ---- drawing
    def fill(self, x0, y0, x1, y1):
        """Fill rectangle, inclusive corners in pixels (fill/copy mode)."""
        self.put(0x36000000 | (x1 * 4) << 12 | (y1 * 4), (x0 * 4) << 12 | (y0 * 4))

    def clear(self, color16):
        self.other_modes(cycle=3)
        self.fill_color(color16 << 16 | color16)
        self.fill(0, 0, W - 1, H - 1)
        self.sync()

    def texrect(self, tile, x0, y0, x1, y1, s, t, dsdx, dtdy):
        """x/y in quarter pixels (10.2), lower-right exclusive in 1/2-cycle mode; s, t in s10.5; dsdx, dtdy in s5.10."""
        # one command of two 64-bit words
        self.put(0x24000000 | x1 << 12 | y1, tile << 24 | x0 << 12 | y0,
                 (s & 0xFFFF) << 16 | (t & 0xFFFF), (dsdx & 0xFFFF) << 16 | (dtdy & 0xFFFF))

    # ---- textures
    def texture(self, addr, data, fmt, size, width, height, tile=0, tmem=0, cs=0, ms=0, masks=0, ct=0, mt=0, maskt=0,
                palette=0):
        """Puts `data` in RDRAM, loads it with LOAD_TILE (LOAD_BLOCK for 4-bit as 8-bit pairs) and sets the tile."""
        self.ram[addr] = bytes(data)
        bpl = (width << size) >> 1
        # 32-bit texels are split between the two TMEM halves: 2 bytes per texel in each line
        line = ((width * 2 if size == SZ_32 else bpl) + 7) // 8
        load_size = SZ_8 if size == SZ_4 else size
        load_w = width // 2 if size == SZ_4 else width
        self.put(0x3D000000 | (FMT_I if size == SZ_4 else fmt) << 21 | load_size << 19 | (load_w - 1), addr)
        self.put(0x35000000 | (FMT_I if size == SZ_4 else fmt) << 21 | load_size << 19 | line << 9 | tmem, 7 << 24)
        self.sync(0x26)
        self.put(0x34000000, 7 << 24 | ((load_w - 1) * 4) << 12 | ((height - 1) * 4))
        self.sync(0x26)
        self.put(0x35000000 | fmt << 21 | size << 19 | line << 9 | tmem,
                 tile << 24 | palette << 20 | ct << 19 | mt << 18 | maskt << 14 | cs << 9 | ms << 8 | masks << 4)
        self.put(0x32000000, tile << 24 | ((width - 1) * 4) << 12 | ((height - 1) * 4))

    def tlut(self, addr, entries16, tmem=256):
        """Palette of 16-bit entries into the upper TMEM half (tmem in 64-bit words: 256 = 0x800)."""
        self.ram[addr] = b''.join(struct.pack('>H', e) for e in entries16)
        self.put(0x3D000000 | FMT_RGBA << 21 | SZ_16 << 19, addr)
        self.put(0x35000000 | tmem, 7 << 24)
        self.sync(0x26)
        self.put(0x30000000, 7 << 24 | ((len(entries16) - 1) * 4) << 12)
        self.sync(0x26)

    # ---- file
    def write(self, path):
        out = bytearray(b'RDPDUMP2')
        out += struct.pack('<II', 8 << 20, 4 << 20)

        def word(v):
            out.extend(struct.pack('<I', v & 0xFFFFFFFF))
        word(8); word(0); word(4 << 20)
        out.extend(b'\x03' * (4 << 20))
        word(9)
        for addr, data in sorted(self.ram.items()):
            data = data + b'\x00' * (-len(data) % 4)
            word(1); word(addr); word(len(data))
            for i in range(0, len(data), 4):
                out.extend(data[i:i + 4][::-1])   # N64 order -> PC words
        word(7)
        for c in self.cmds + [[0x29000000, 0]]:
            word(2); word((c[0] >> 24) & 0x3F); word(len(c))
            for v in c:
                word(v)
        vi = [0x00003016, COLOR, W, 2, 0, 0x03E52239, 0x20D, 0x0C15, 0x0C150C15, 0x006C02EC, 0x002501FF, 0x000E0204,
              0x200, 0x400]
        for i, v in enumerate(vi):
            word(3); word(i); word(v)
        word(4); word(6)
        with open(path, 'wb') as f:
            f.write(out)


# ---- texture data
def tex_rgba16(w, h, rnd, kind):
    px = []
    for y in range(h):
        for x in range(w):
            if kind == 'checker':
                c = (31, 31, 31) if (x // 4 + y // 4) & 1 else (2, 6, 20)
                a = 1
            elif kind == 'gradient':
                c = (x * 31 // (w - 1), y * 31 // (h - 1), (x + y) * 31 // (w + h - 2))
                a = 1 if (x + y) % 5 else 0
            else:
                c = (rnd.randrange(32), rnd.randrange(32), rnd.randrange(32))
                a = rnd.randrange(2)
            px.append(c[0] << 11 | c[1] << 6 | c[2] << 1 | a)
    return b''.join(struct.pack('>H', p) for p in px)


def tex_rgba32(w, h, rnd):
    return bytes(rnd.randrange(256) for _ in range(w * h * 4))


def tex_bytes(n, rnd):
    return bytes(rnd.randrange(256) for _ in range(n))


def rgba(rnd, alpha=None):
    return rnd.randrange(1 << 24) << 8 | (rnd.randrange(256) if alpha is None else alpha)


TEX0_TIMES_PRIM = ((A_TEX0, B_ZERO, C_PRIM, D_ZERO), (AA_TEX0, AA_ZERO, AA_PRIM, AA_ZERO))
TEX0_ONLY = ((A_ZERO, B_ZERO, C_ZERO, D_TEX0), (AA_ZERO, AA_ZERO, AA_ZERO, AA_TEX0))


# ---- cases: (name, category, description, builder(scene, rnd))
def filt_case(filt, scale):
    def build(s, rnd):
        s.clear(0x0001)
        s.texture(TEXBASE, tex_rgba16(32, 32, rnd, 'gradient' if scale < 1 else 'checker'), FMT_RGBA, SZ_16, 32, 32,
                  masks=5, maskt=5)
        s.other_modes(filt=filt)
        s.combine(*TEX0_ONLY)
        s.sync()
        d = int(1024 / scale)
        # a grid of rectangles at fractional starting texels
        for i in range(6):
            x0, y0 = 8 + (i % 3) * 100, 8 + (i // 3) * 110
            st = rnd.randrange(0, 32 * 32)
            s.texrect(0, x0 * 4, y0 * 4, (x0 + 96) * 4, (y0 + 100) * 4, st, (st * 7) & 0x3FF, d + i * 37, d + i * 53)
    return build


def fmt_case(fmt, size):
    def build(s, rnd):
        s.clear(0x0001)
        w, h = 32, 16
        if fmt == FMT_CI:
            n = 16 if size == SZ_4 else 256
            s.tlut(TEXBASE + 0x8000, [rnd.randrange(65536) | 1 for _ in range(n)])
            data = tex_bytes((w * h) >> (1 if size == SZ_4 else 0), rnd)
        elif size == SZ_32:
            data = tex_rgba32(w, h, rnd)
        elif size == SZ_16:
            data = tex_rgba16(w, h, rnd, 'random') if fmt == FMT_RGBA else tex_bytes(w * h * 2, rnd)
        elif size == SZ_8:
            data = tex_bytes(w * h, rnd)
        else:
            data = tex_bytes(w * h // 2, rnd)
        s.texture(TEXBASE, data, fmt, size, w, h)
        s.other_modes(filt=FILT_POINT, tlut=1 if fmt == FMT_CI else 0)
        s.combine(*TEX0_ONLY)
        s.sync()
        s.texrect(0, 16 * 4, 16 * 4, (16 + 256) * 4, (16 + 128) * 4, 0, 0, 1024 // 8, 1024 // 8)
    return build


def combiner_case(two_cycle):
    def build(s, rnd):
        s.clear(0x0001)
        s.texture(TEXBASE, tex_rgba16(32, 32, rnd, 'random'), FMT_RGBA, SZ_16, 32, 32)
        for i in range(8):
            s.prim(rgba(rnd))
            s.env(rgba(rnd))
            ca = (rnd.choice([A_TEX0, A_PRIM, A_ENV, A_ONE, A_ZERO]), rnd.choice([B_TEX0, B_PRIM, B_ENV, B_ZERO]),
                  rnd.choice([C_TEX0, C_PRIM, C_ENV, C_TEX0A, C_PRIMA, C_ENVA, C_ZERO]),
                  rnd.choice([D_TEX0, D_PRIM, D_ENV, D_ONE, D_ZERO]))
            aa = (rnd.choice([AA_TEX0, AA_PRIM, AA_ENV, AA_ONE]), rnd.choice([AA_TEX0, AA_PRIM, AA_ZERO]),
                  rnd.choice([AA_TEX0, AA_PRIM, AA_ENV]), rnd.choice([AA_TEX0, AA_PRIM, AA_ZERO]))
            if two_cycle:
                cb = (rnd.choice([A_COMB, A_PRIM, A_ENV]), rnd.choice([B_COMB, B_ENV, B_ZERO]),
                      rnd.choice([C_COMB, C_PRIM, C_ENVA, C_ZERO]), rnd.choice([D_COMB, D_ENV, D_ZERO]))
                ab = (AA_COMB, AA_ZERO, AA_ONE, AA_ZERO)
                s.other_modes(cycle=1)
                s.combine(ca, aa, cb, ab)
            else:
                s.other_modes(cycle=0)
                s.combine(ca, aa)
            s.sync()
            x0, y0 = 8 + (i % 4) * 78, 8 + (i // 4) * 115
            s.texrect(0, x0 * 4, y0 * 4, (x0 + 72) * 4, (y0 + 108) * 4, 0, 0, 512, 512)
    return build


def blend_case(mode):
    def build(s, rnd):
        s.clear(0x5295)
        s.texture(TEXBASE, tex_rgba16(32, 32, rnd, 'random'), FMT_RGBA, SZ_16, 32, 32)
        for i in range(6):
            s.prim(rgba(rnd))
            s.blend_color(rgba(rnd))
            # P*A + M*(1-A) in both cycles: m1a pixel (0), m1b pixel alpha (0), m2a memory (1), m2b 1-A (0);
            # other_modes' `blend` holds bits 16-31 of the low word: m2a_0 at bit 6, m2a_1 at bit 4.
            blend = 1 << 6 | 1 << 4
            if mode == 'alpha':
                s.other_modes(blend=blend, force_blend=1, image_read=1)
                s.combine((A_TEX0, B_ZERO, C_PRIM, D_ZERO), (AA_TEX0, AA_ZERO, AA_PRIM, AA_ZERO))
            elif mode == 'compare':
                s.other_modes(alpha_compare=1)
                s.combine((A_TEX0, B_ZERO, C_PRIM, D_ZERO), (AA_PRIM, AA_ZERO, AA_TEX0, AA_ZERO))
            else:   # coverage: cvg times alpha, alpha cvg select
                s.other_modes(blend=blend, force_blend=0, image_read=1, cvg_times_alpha=1, alpha_cvg_select=1,
                              cvg_dest=1)
                s.combine((A_TEX0, B_ZERO, C_PRIM, D_ZERO), (AA_TEX0, AA_ZERO, AA_PRIM, AA_ZERO))
            s.sync()
            x0, y0 = 10 + (i % 3) * 100, 10 + (i // 3) * 112
            s.texrect(0, x0 * 4, y0 * 4, (x0 + 120) * 4, (y0 + 100) * 4, 0, 0, 300, 300)
    return build


def rtt_case(s, rnd):
    # pass 1: a small 64x64 image drawn with a texture, pass 2: that image used as a texture, scaled
    s.color_image(RTT, 64)
    s.scissor(0, 0, 64, 64)
    s.other_modes(cycle=3)
    s.fill_color(0x07C107C1)
    s.fill(0, 0, 63, 63)
    s.sync()
    s.texture(TEXBASE, tex_rgba16(32, 32, rnd, 'checker'), FMT_RGBA, SZ_16, 32, 32)
    s.other_modes(filt=FILT_BILERP)
    s.combine(*TEX0_ONLY)
    s.sync()
    s.texrect(0, 8 * 4, 8 * 4, 56 * 4, 56 * 4, 0, 0, 700, 700)
    s.sync(0x29)
    s.color_image()
    s.scissor()
    s.clear(0x0001)
    s.put(0x3D000000 | FMT_RGBA << 21 | SZ_16 << 19 | 63, RTT)
    s.put(0x35000000 | FMT_RGBA << 21 | SZ_16 << 19 | 16 << 9, 7 << 24)   # 64 texels x 2 bytes = 16 words a line
    s.sync(0x26)
    s.put(0x34000000, 7 << 24 | (63 * 4) << 12 | (31 * 4))   # rows 0-31 of the drawn image (TMEM holds 4 KB)
    s.sync(0x26)
    s.put(0x35000000 | FMT_RGBA << 21 | SZ_16 << 19 | 16 << 9, 1 << 19 | 1 << 9)   # clamped in s and t
    s.put(0x32000000, (63 * 4) << 12 | (31 * 4))
    s.other_modes(filt=FILT_BILERP)
    s.combine(*TEX0_ONLY)
    s.sync()
    s.texrect(0, 20 * 4, 20 * 4, 300 * 4, 160 * 4, 0, 0, 230, 230)


CASES = [
    ('filter_point_up', 'filter', 'point sampling, magnified 3-4x at fractional texels', filt_case(FILT_POINT, 3.5)),
    ('filter_bilerp_up', 'filter', '3-point filter, magnified 3-4x', filt_case(FILT_BILERP, 3.5)),
    ('filter_average_up', 'filter', 'average filter (box), magnified', filt_case(FILT_AVERAGE, 3.5)),
    ('filter_bilerp_down', 'filter', '3-point filter, minified 0.7x', filt_case(FILT_BILERP, 0.7)),
    ('format_rgba16', 'format', 'RGBA 16-bit, point', fmt_case(FMT_RGBA, SZ_16)),
    ('format_rgba32', 'format', 'RGBA 32-bit, point', fmt_case(FMT_RGBA, SZ_32)),
    ('format_ia16', 'format', 'IA 16-bit', fmt_case(FMT_IA, SZ_16)),
    ('format_ia8', 'format', 'IA 8-bit', fmt_case(FMT_IA, SZ_8)),
    ('format_ia4', 'format', 'IA 4-bit', fmt_case(FMT_IA, SZ_4)),
    ('format_i8', 'format', 'I 8-bit', fmt_case(FMT_I, SZ_8)),
    ('format_i4', 'format', 'I 4-bit', fmt_case(FMT_I, SZ_4)),
    ('format_ci8', 'format', 'CI 8-bit, RGBA16 palette', fmt_case(FMT_CI, SZ_8)),
    ('format_ci4', 'format', 'CI 4-bit, RGBA16 palette', fmt_case(FMT_CI, SZ_4)),
    ('combiner_1cycle', 'combiner', 'random 1-cycle combiners with prim/env', combiner_case(False)),
    ('combiner_2cycle', 'combiner', 'random 2-cycle combiners', combiner_case(True)),
    ('blend_alpha', 'blend', 'force blend, pixel alpha over memory', blend_case('alpha')),
    ('blend_compare', 'blend', 'alpha compare against the blend colour', blend_case('compare')),
    ('blend_coverage', 'blend', 'coverage times alpha, alpha coverage select', blend_case('coverage')),
    ('rtt_texture', 'rtt', 'a 64x64 image drawn, then used as a texture', rtt_case),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--filter', default='')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--random', type=int, default=0, help='extra seeds per case (seed+1 .. seed+N)')
    ap.add_argument('--score', action='store_true')
    ap.add_argument('--png', action='store_true')
    a = ap.parse_args()
    n = 0
    for name, cat, desc, build in CASES:
        if a.filter and a.filter not in name:
            continue
        for k in range(a.random + 1):
            seed = a.seed + k
            cname = name if k == 0 else '%s_s%d' % (name, seed)
            s = Scene()
            s.color_image()
            s.scissor()
            build(s, random.Random(seed * 7919 + sum(map(ord, name))))
            d = os.path.join(a.out, cname)
            os.makedirs(d, exist_ok=True)
            s.write(os.path.join(d, 'scene.rdp'))
            json.dump({'name': cname, 'category': cat, 'description': desc, 'seed': seed},
                      open(os.path.join(d, 'case.json'), 'w'), indent=1)
            n += 1
    print('%d cases in %s' % (n, a.out))
    if a.score:
        import glob
        import subprocess
        import sys
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        h64test = os.path.join(root, 'build-msvc', 'h64test.exe')
        oracle = r'E:\IA-labs\Xenon64\build-oracle\rdp-oracle.exe'
        for d in sorted(glob.glob(os.path.join(a.out, '*', 'scene.rdp'))):
            case = os.path.dirname(d)
            if a.filter and a.filter not in os.path.basename(case):
                continue
            subprocess.run([h64test, '--rt-play', d, case, '--frames', 'all'], capture_output=True)
            subprocess.run([oracle, d, case, '--frames', 'all'], capture_output=True)
            cmp = [sys.executable, os.path.join(root, 'tools', 'rt_compare.py'), d, case]
            if a.png:
                cmp.append('--png')
            r = subprocess.run(cmp, capture_output=True, text=True)
            print('== %s\n%s' % (os.path.basename(case), r.stdout.rstrip()))
            for f in glob.glob(os.path.join(case, '*.rdram')) + glob.glob(os.path.join(case, '*.hidden')):
                os.remove(f)
        subprocess.run([sys.executable, os.path.join(root, 'tools', 'rt_report.py'), a.out])


if __name__ == '__main__':
    main()
