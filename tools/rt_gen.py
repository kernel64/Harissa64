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

COLOR, TEXBASE, RTT, ZBUF = 0x100000, 0x400000, 0x300000, 0x200000
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
                    cvg_dest=0, aa=0, color_on_cvg=0, z=0, z_mode=0, tex_lod=0):
        """z: 1 compare and update (per-pixel depth); tex_lod: texture LOD (mip-mapping, bit 16 of the high word)."""
        hi = (cycle << 20 | persp << 19 | tex_lod << 16 | tlut << 15 | tlut_ia << 14 | filt << 12 | 6 << 9 |
              rgb_dither << 6 | alpha_dither << 4)
        lo = (blend << 16 | force_blend << 14 | alpha_cvg_select << 13 | cvg_times_alpha << 12 | z_mode << 10 |
              cvg_dest << 8 | color_on_cvg << 7 | image_read << 6 | (z << 5) | (z << 4) | aa << 3 | alpha_compare)
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

    def depth_image(self, addr=ZBUF):
        self.put(0x3E000000, addr)

    def clear_depth(self, addr=ZBUF):
        """Fills the depth image with the farthest depth (0xFFFC), through a fill into it."""
        self.color_image(addr)
        self.other_modes(cycle=3)
        self.fill_color(0xFFFCFFFC)
        self.fill(0, 0, W - 1, H - 1)
        self.sync()
        self.color_image()

    def tri(self, v0, v1, v2, shade=True, texture=False, zbuf=False, tile=0, levels=1):
        """An HLE triangle record (render/api.h vertices: x, y in pixels, z 0..0x7FFF, invw, r g b a 0..255,
        s, t in s10.5 units before perspective; levels: mip-map levels). h64test --rt-tris adds the RDP command."""
        flags = (1 if zbuf else 0) | (2 if texture else 0) | (4 if shade else 0)
        self.cmds.append({'tri': (v0, v1, v2), 'flags': flags, 'tile': tile, 'levels': levels})

    # ---- textures
    def texture(self, addr, data, fmt, size, width, height, tile=0, tmem=0, cs=0, ms=0, masks=0, ct=0, mt=0, maskt=0,
                palette=0, shifts=0, shiftt=0):
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
                 tile << 24 | palette << 20 | ct << 19 | mt << 18 | maskt << 14 | shiftt << 10 | cs << 9 | ms << 8 |
                 masks << 4 | shifts)
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
            if isinstance(c, dict):
                word(0x48); word(c['flags']); word(c['tile']); word(c.get('levels', 1))
                for v in c['tri']:
                    out.extend(struct.pack('<10f', v['x'], v['y'], v.get('z', 0.0), v.get('invw', 1.0),
                                           v.get('r', 255.0), v.get('g', 255.0), v.get('b', 255.0), v.get('a', 255.0),
                                           v.get('s', 0.0), v.get('t', 0.0)))
                continue
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


SHADE_ONLY = ((A_ZERO, B_ZERO, C_ZERO, D_SHADE), (AA_ZERO, AA_ZERO, AA_ZERO, AA_SHADE))
TEX0_TIMES_SHADE = ((A_TEX0, B_ZERO, C_SHADE, D_ZERO), (AA_TEX0, AA_ZERO, AA_SHADE, AA_ZERO))
# blender (other_modes' `blend`: m1a_0<<14 m1a_1<<12 m1b_0<<10 m1b_1<<8 m2a_0<<6 m2a_1<<4 m2b_0<<2 m2b_1)
BL_FOG_SHADE = 3 << 14 | 0 << 12 | 2 << 10 | 3 << 8 | 0 << 6 | 0 << 4 | 0 << 2 | 2   # fog*shade a + in*(1-a), then in
BL_AA_OPA = 1 << 6 | 1 << 4 | 1 << 2 | 1                                               # in*cvg + mem*(1-cvg)


def vtx(rnd, x0=0, y0=0, x1=W, y1=H, z=None, alpha=255):
    return {'x': rnd.uniform(x0, x1), 'y': rnd.uniform(y0, y1), 'z': z if z is not None else rnd.uniform(0, 0x7FFF),
            'r': float(rnd.randrange(256)), 'g': float(rnd.randrange(256)), 'b': float(rnd.randrange(256)),
            'a': float(alpha)}


def tri_shade(s, rnd):
    s.clear(0x0001)
    s.other_modes()
    s.combine(*SHADE_ONLY)
    s.sync()
    for _ in range(24):
        s.tri(vtx(rnd), vtx(rnd), vtx(rnd))


def tri_zbuffer(s, rnd):
    s.depth_image()
    s.clear_depth()
    s.clear(0x0001)
    s.other_modes(z=1)
    s.combine(*SHADE_ONLY)
    s.sync()
    for _ in range(16):
        s.tri(vtx(rnd), vtx(rnd), vtx(rnd), zbuf=True)


def quad_persp(s, rnd, x0, y0, x1, y1, near, far, texels=32, shade=False):
    """A floor-like quad: the top edge far (small 1/w), the bottom near."""
    def v(x, y, w, sc, tc):
        return {'x': x, 'y': y, 'z': 0x7FFF * (1 - w), 'invw': w, 's': sc * 32.0, 't': tc * 32.0,
                'r': 255.0, 'g': 255.0, 'b': 255.0, 'a': 255.0}
    inset = (x1 - x0) * 0.3
    a = v(x0 + inset, y0, far, 0, 0)
    b = v(x1 - inset, y0, far, texels, 0)
    c = v(x1, y1, near, texels, texels)
    d = v(x0, y1, near, 0, texels)
    s.tri(a, b, c, shade=shade, texture=True)
    s.tri(a, c, d, shade=shade, texture=True)


def tri_persp(filt):
    def build(s, rnd):
        s.clear(0x0001)
        s.texture(TEXBASE, tex_rgba16(32, 32, rnd, 'checker'), FMT_RGBA, SZ_16, 32, 32, masks=5, maskt=5)
        s.other_modes(filt=filt, persp=1)
        s.combine(*TEX0_ONLY)
        s.sync()
        quad_persp(s, rnd, 10, 10, 310, 115, 1.0, 0.25, texels=64)
        quad_persp(s, rnd, 40, 125, 280, 230, 0.9, 0.6, texels=32)
    return build


def tri_fog(s, rnd):
    s.clear(0x0001)
    s.put(0x38000000, rgba(rnd, 255))   # SET_FOG_COLOR
    s.other_modes(cycle=1, blend=BL_FOG_SHADE, force_blend=1)
    s.combine((A_ZERO, B_ZERO, C_ZERO, D_SHADE), (AA_ZERO, AA_ZERO, AA_ZERO, AA_SHADE),
              (A_ZERO, B_ZERO, C_ZERO, D_COMB), (AA_ZERO, AA_ZERO, AA_ZERO, AA_COMB))
    s.sync()
    for _ in range(16):
        s.tri(vtx(rnd, alpha=rnd.randrange(256)), vtx(rnd, alpha=rnd.randrange(256)), vtx(rnd, alpha=rnd.randrange(256)))


def tri_aa(s, rnd):
    s.clear(0x0001)
    s.other_modes(aa=1, image_read=1, alpha_cvg_select=1, blend=BL_AA_OPA, cvg_dest=0)
    s.combine(*SHADE_ONLY)
    s.sync()
    for _ in range(20):
        s.tri(vtx(rnd), vtx(rnd), vtx(rnd))


def tri_tex_shade(s, rnd):
    s.clear(0x0001)
    s.texture(TEXBASE, tex_rgba16(32, 32, rnd, 'gradient'), FMT_RGBA, SZ_16, 32, 32, masks=5, maskt=5)
    s.other_modes(filt=FILT_BILERP)
    s.combine(*TEX0_TIMES_SHADE)
    s.sync()
    for _ in range(12):
        v = [vtx(rnd) for _ in range(3)]
        for p in v:
            p['s'] = rnd.uniform(0, 64) * 32
            p['t'] = rnd.uniform(0, 64) * 32
        s.tri(v[0], v[1], v[2], texture=True)


def tri_small(s, rnd):
    s.clear(0x0001)
    s.other_modes()
    s.combine(*SHADE_ONLY)
    s.sync()
    for i in range(300):
        cx, cy = rnd.uniform(4, W - 4), rnd.uniform(4, H - 4)
        s.tri(*[dict(vtx(rnd), x=cx + rnd.uniform(-3, 3), y=cy + rnd.uniform(-3, 3)) for _ in range(3)])


def tri_mip(filt):
    """A receding floor with 6 mip levels (32x32 down to 1x1, tiles 0-5, each level its own tint over a checker),
    LOD on, 2 cycles: (TEXEL1 - TEXEL0) * LOD_FRAC + TEXEL0, then the combined colour."""
    def build(s, rnd):
        s.clear(0x0001)
        tints = [(31, 31, 31), (31, 8, 8), (8, 31, 8), (8, 8, 31), (31, 31, 8), (8, 31, 31)]
        tmem = 0
        for lv in range(6):
            n = 32 >> lv
            px = []
            for y in range(n):
                for xx in range(n):
                    tr, tg, tb = tints[lv]
                    k = 1 if ((xx // max(1, n // 8)) + (y // max(1, n // 8))) & 1 else 2
                    px.append((tr // k) << 11 | (tg // k) << 6 | (tb // k) << 1 | 1)
            data = b''.join(struct.pack('>H', p) for p in px)
            s.texture(TEXBASE + lv * 0x1000, data, FMT_RGBA, SZ_16, n, n, tile=lv, tmem=tmem, masks=5 - lv, maskt=5 - lv,
                      shifts=lv, shiftt=lv)
            tmem += max(1, (n * 2 + 7) // 8) * n
        s.other_modes(cycle=1, filt=filt, persp=1, tex_lod=1)
        lerp = ((A_TEX1, B_TEX0, 13, D_TEX0), (AA_TEX1, AA_TEX0, 0, AA_TEX0))   # c 13 / alpha c 0: LOD fraction
        s.combine(lerp[0], lerp[1], (A_ZERO, B_ZERO, C_ZERO, D_COMB), (AA_ZERO, AA_ZERO, AA_ZERO, AA_COMB))
        s.sync()

        def v(x, y, w, sc, tc):
            return {'x': x, 'y': y, 'z': 0x7FFF * (1 - w), 'invw': w, 's': sc * 32.0, 't': tc * 32.0}
        a, b = v(150, 8, 0.03, 0, 0), v(170, 8, 0.03, 256, 0)
        c, d = v(318, 236, 1.0, 256, 256), v(2, 236, 1.0, 0, 256)
        s.tri(a, b, c, shade=False, texture=True, levels=6)
        s.tri(a, c, d, shade=False, texture=True, levels=6)
    return build


def tri_detail_shift(filt):
    """ISS 64's grass (render test iss64_user1): two textures blended by the shade colour, the second a small
    noise tile magnified by its shift (12: coordinates x16), 2 cycles:
    cycle 0 (TEXEL0 - TEXEL1) * SHADE + TEXEL1, cycle 1 (COMBINED - TEXEL1) * PRIM + TEXEL1 (texels swapped there)."""
    def build(s, rnd):
        s.clear(0x0001)
        s.texture(TEXBASE, tex_rgba16(32, 32, rnd, 'gradient'), FMT_RGBA, SZ_16, 32, 32, tile=0, tmem=0,
                  masks=5, maskt=5, shifts=1, shiftt=1)
        s.texture(TEXBASE + 0x1000, tex_rgba16(32, 32, rnd, 'checker'), FMT_RGBA, SZ_16, 32, 32, tile=1, tmem=0x100,
                  masks=5, maskt=5, shifts=12, shiftt=12)
        s.put(0x3A000000, 0x808080E8)   # SET_PRIM_COLOR, as the game
        s.other_modes(cycle=1, filt=filt, persp=1)
        s.combine((A_TEX0, B_TEX1, C_SHADE, D_TEX1), (AA_ZERO, AA_ZERO, AA_ZERO, AA_ONE),
                  (A_COMB, B_TEX1, C_PRIM, D_TEX1), (AA_ZERO, AA_ZERO, AA_ZERO, AA_COMB))
        s.sync()
        for k in range(2):
            v = [vtx(rnd) for _ in range(3)]
            for p in v:
                p['s'] = rnd.uniform(0, 128) * 32
                p['t'] = rnd.uniform(0, 128) * 32
            s.tri(v[0], v[1], v[2], texture=True)
        quad_persp(s, rnd, 10, 120, 310, 236, 1.0, 0.3, texels=128, shade=True)
    return build


def fmt_ci4_pal(s, rnd):
    """CI4 through palette 1 (TLUT entries 16..31): format_ci4 only uses palette 0."""
    s.clear(0x0001)
    s.tlut(TEXBASE + 0x8000, [rnd.randrange(65536) | 1 for _ in range(32)])
    s.texture(TEXBASE, tex_bytes(32 * 16 // 2, rnd), FMT_CI, SZ_4, 32, 16, palette=1)
    s.other_modes(filt=FILT_POINT, tlut=1)
    s.combine(*TEX0_ONLY)
    s.sync()
    s.texrect(0, 16 * 4, 16 * 4, (16 + 256) * 4, (16 + 128) * 4, 0, 0, 1024 // 8, 1024 // 8)


def tri_detail_ci(filt):
    """tri_detail_shift with ISS 64's own formats: tile 0 CI4 palette 1 (shift 1), tile 1 CI4 palette 0
    (shift 12), the palette in TMEM's upper half."""
    def build(s, rnd):
        s.clear(0x0001)
        s.tlut(TEXBASE + 0x8000, [rnd.randrange(65536) | 1 for _ in range(32)])
        s.texture(TEXBASE, tex_bytes(32 * 32 // 2, rnd), FMT_CI, SZ_4, 32, 32, tile=0, tmem=0,
                  masks=5, maskt=5, shifts=1, shiftt=1, palette=1)
        s.texture(TEXBASE + 0x1000, tex_bytes(32 * 32 // 2, rnd), FMT_CI, SZ_4, 32, 32, tile=1, tmem=0xC0,
                  masks=5, maskt=5, shifts=12, shiftt=12, palette=0)
        s.put(0x3A000000, 0x808080E8)
        s.other_modes(cycle=1, filt=filt, persp=1, tlut=1)
        s.combine((A_TEX0, B_TEX1, C_SHADE, D_TEX1), (AA_ZERO, AA_ZERO, AA_ZERO, AA_ONE),
                  (A_COMB, B_TEX1, C_PRIM, D_TEX1), (AA_ZERO, AA_ZERO, AA_ZERO, AA_COMB))
        s.sync()
        quad_persp(s, rnd, 10, 20, 310, 236, 1.0, 0.3, texels=128, shade=True)
    return build


CASES = [
    ('format_ci4_pal1', 'format', 'CI 4-bit through palette 1', fmt_ci4_pal),
    ('tri_detail_ci_point', 'combiner', 'ISS 64 grass with its CI4 textures (palettes 1 and 0), point',
     tri_detail_ci(FILT_POINT)),
    ('tri_detail_ci_bilerp', 'combiner', 'ISS 64 grass with its CI4 textures (palettes 1 and 0), 3-point filter',
     tri_detail_ci(FILT_BILERP)),
    ('tri_detail_point', 'combiner', 'two textures blended by shade, the second shifted x16 (ISS 64 grass), point',
     tri_detail_shift(FILT_POINT)),
    ('tri_detail_bilerp', 'combiner', 'two textures blended by shade, the second shifted x16, 3-point filter',
     tri_detail_shift(FILT_BILERP)),
    ('tri_mip_point', 'lod', 'mip-mapped receding floor, 6 levels, point sampling', tri_mip(FILT_POINT)),
    ('tri_mip_bilerp', 'lod', 'mip-mapped receding floor, 6 levels, 3-point filter', tri_mip(FILT_BILERP)),
    ('tri_shade', 'triangle', 'Gouraud-shaded triangles, 1 cycle', tri_shade),
    ('tri_zbuffer', 'triangle', 'shaded triangles through each other, depth compare and update', tri_zbuffer),
    ('tri_persp_point', 'triangle', 'perspective-corrected textured floors, point sampling', tri_persp(FILT_POINT)),
    ('tri_persp_bilerp', 'triangle', 'perspective-corrected textured floors, 3-point filter', tri_persp(FILT_BILERP)),
    ('tri_fog', 'triangle', 'fog colour blended by shade alpha (2 cycles)', tri_fog),
    ('tri_aa', 'triangle', 'antialiased edges: coverage blended with memory', tri_aa),
    ('tri_tex_shade', 'triangle', 'texture times shade, 3-point filter', tri_tex_shade),
    ('tri_small', 'triangle', '300 triangles of a few pixels (edge rules, sub-pixel positions)', tri_small),
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
            if any(isinstance(c, dict) for c in s.cmds):
                # the RDP triangle commands, built by our triangle setup as on the HLE path
                import subprocess
                root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
                p = os.path.join(d, 'scene.rdp')
                r = subprocess.run([os.path.join(root, 'build-msvc', 'h64test.exe'), '--rt-tris', p, p],
                                   capture_output=True, text=True)
                if r.returncode:
                    raise SystemExit('h64test --rt-tris failed: ' + r.stdout + r.stderr)
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
