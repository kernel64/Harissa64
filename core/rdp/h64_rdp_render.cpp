// Harissa64 V2 - software RDP: commands, rasterisation and the pixel pipeline.
//
// Serial CPU port of ParaLLEl-RDP (MIT, see h64_rdp_state.h): command
// decoding (rdp_device.cpp), derived state (rdp_renderer.cpp), span setup
// (shaders/span_setup.comp), per-pixel shading (shading.h, interpolation.h,
// perspective.h, coverage.h, combiner.h, dither.h, noise.h) and the output
// stage (depth_blend.comp, memory_interfacing.h, depth_test.h, blender.h,
// z_encode.h, clamping.h). Upscaling paths are dropped (scale factor 1).
// Primitives are drawn one after the other, each pixel read from and
// written back to RDRAM immediately, which gives the RDP's in-order result.
#include "h64_rdp.h"
#include "h64_rdp_state.h"
#include "h64_rdp_luts.h"

#include <stdlib.h>
#include <string.h>

#include "../common/h64_log.h"
#include "../system/h64_system.h"

enum { TS_FLIP = 1, TS_DO_OFFSET = 2, TS_SKIP_XFRAC = 4, TS_INTERLACE_FIELD = 8, TS_INTERLACE_KEEP_ODD = 16 };

struct TriSetup
{
    s32 xh, xm, xl;
    s32 yh, ym, yl;
    s32 dxhdy, dxmdy, dxldy;
    u32 flags, tile;
};

struct AttrSetup
{
    s32 rgba[4], drgbaDx[4], drgbaDe[4], drgbaDy[4];
    s32 stzw[4], dstzwDx[4], dstzwDe[4], dstzwDy[4];   // s, t, z, w
};

struct Span
{
    s32 rgba[4], stzw[4];
    u16 xleft[4], xright[4];
    s32 baseX, startX, endX;
    s32 lodLength;
    int valid;
};

// Per-primitive derived state (rdp_renderer.cpp build_derived_attributes)
struct Derived
{
    s32 dz, dzCompressed;
    s32 factors[4];
    s32 minLod;
    int usesTexel0, usesTexel1, usesPipelinedTexel1, usesLod, needNoise, needNoiseDual;
    u32 flags;   // normalised raster flags
};

struct Pixel   // one pixel of the colour and depth images, as the RDP sees it
{
    u8 c[4];
    u16 depth;
    u8 dz;
    int colorDirty, depthDirty;
    u32 colorIndex, depthIndex;
};

// ---- Small helpers ----
static s32 min4(const u16 *v) { s32 m = v[0]; int i; for (i = 1; i < 4; i++) if (v[i] < m) m = v[i]; return m; }
static s32 max4(const u16 *v) { s32 m = v[0]; int i; for (i = 1; i < 4; i++) if (v[i] > m) m = v[i]; return m; }
static s32 clamp9(s32 c) { return h64_clamp(h64_sext(c - 0x80, 9) + 0x80, 0, 0xFF); }

static s32 clamp_z(s32 z)
{
    z -= 1 << 17;
    z = (s32)((u32)z << 13) >> 13;
    z += 1 << 17;
    return h64_clamp(z, 0, 0x3FFFF);
}

// ---- Z encoding (z_encode.h) ----
static s32 z_decompress(u32 z)
{
    s32 exponent = (s32)(z >> 11), mantissa = (s32)(z & 0x7FF);
    s32 shift = 6 - exponent > 0 ? 6 - exponent : 0;
    s32 base = 0x40000 - (0x40000 >> exponent);
    return (mantissa << shift) + base;
}
static u16 z_compress(s32 z)
{
    s32 inv = 0x3FFFF - z > 1 ? 0x3FFFF - z : 1;
    s32 exponent = h64_clamp(17 - h64_find_msb((u32)inv), 0, 7);
    s32 shift = 6 - exponent > 0 ? 6 - exponent : 0;
    return (u16)((exponent << 11) + ((z >> shift) & 0x7FF));
}
static s32 dz_compress_msb(s32 dz) { int m = h64_find_msb((u32)dz); return m > 0 ? m : 0; }

// rdp_renderer.cpp dz_compress / normalize_dzpix
static s32 dz_compress_pot(s32 dz)
{
    s32 v = 0;
    if (dz & 0xFF00) v |= 8;
    if (dz & 0xF0F0) v |= 4;
    if (dz & 0xCCCC) v |= 2;
    if (dz & 0xAAAA) v |= 1;
    return v;
}
static s32 normalize_dzpix(s32 dz)
{
    if (dz >= 0x8000) return 0x8000;
    if (dz == 0) return 1;
    return 1 << (h64_find_msb((u32)dz) + 1);
}

// ---- Noise (noise.h) ----
static void reseed_noise(H64RdpState *st, u32 x, u32 y, u32 prim)
{
    const u32 P = 1103515245u;
    u32 s0 = x, s1 = y, s2 = prim, n0, n1, n2, r;
    for (r = 0; r < 3; r++)
    {
        n0 = ((s0 >> 8) ^ s1) * P;
        n1 = ((s1 >> 8) ^ s2) * P;
        n2 = ((s2 >> 8) ^ s0) * P;
        s0 = n0; s1 = n1; s2 = n2;
    }
    st->noise = (u16)(s0 >> 16);
}

// ---- State setup ----
static H64RdpState *state(H64System *sys)
{
    if (!sys->rdpState)
    {
        sys->rdpState = (H64RdpState *)calloc(1, sizeof(H64RdpState));
        sys->rdramHidden = (u8 *)malloc(H64_RDRAM_SIZE / 2);
        memset(sys->rdramHidden, 3, H64_RDRAM_SIZE / 2);
        sys->rdpState->colorFmt = FB_RGBA5551;
        sys->rdpState->colorWidth = 320;
    }
    return sys->rdpState;
}

H64RdpState *h64_rdp_state(H64System *sys) { return state(sys); }

static int fb_size_of(int fmt)
{
    switch (fmt)
    {
    case FB_I4: return 0;
    case FB_I8: return 1;
    case FB_RGBA8888: return 4;
    default: return 2;
    }
}

// ---- Framebuffer access (memory_interfacing.h) ----
static void load_pixel(H64System *sys, H64RdpState *st, u32 x, u32 y, Pixel *p)
{
    u8 *r = sys->rdram, *hid = sys->rdramHidden;
    u32 w = st->colorWidth, idx, word;
    p->colorDirty = p->depthDirty = 0;
    switch (st->colorFmt)
    {
    case FB_I4: case FB_I8:
        idx = ((st->colorAddr) + w * y + x) & (H64_RDRAM_SIZE - 1);
        p->c[0] = p->c[1] = p->c[2] = r[idx];
        p->c[3] = hid[idx >> 1];
        break;
    case FB_RGBA5551:
        idx = ((st->colorAddr >> 1) + w * y + x) & (H64_RDRAM_SIZE / 2 - 1);
        word = (u32)r[idx * 2] << 8 | r[idx * 2 + 1];
        p->c[0] = (u8)((word >> 8) & 0xF8); p->c[1] = (u8)((word >> 3) & 0xF8); p->c[2] = (u8)((word << 2) & 0xF8);
        p->c[3] = (u8)((hid[idx] << 5) | ((word & 1) << 7));
        break;
    case FB_IA88:
        idx = ((st->colorAddr >> 1) + w * y + x) & (H64_RDRAM_SIZE / 2 - 1);
        word = (u32)r[idx * 2] << 8 | r[idx * 2 + 1];
        p->c[0] = p->c[1] = p->c[2] = (u8)(word >> 8);
        p->c[3] = (u8)word;
        break;
    default:
        idx = ((st->colorAddr >> 2) + w * y + x) & (H64_RDRAM_SIZE / 4 - 1);
        p->c[0] = r[idx * 4]; p->c[1] = r[idx * 4 + 1]; p->c[2] = r[idx * 4 + 2]; p->c[3] = r[idx * 4 + 3];
        break;
    }
    p->colorIndex = idx;
    idx = ((st->depthAddr >> 1) + w * y + x) & (H64_RDRAM_SIZE / 2 - 1);
    word = (u32)r[idx * 2] << 8 | r[idx * 2 + 1];
    p->depth = (u16)(word >> 2);
    p->dz = (u8)(hid[idx] | ((word & 3) << 2));
    p->depthIndex = idx;
}

static void store_pixel(H64System *sys, H64RdpState *st, const Pixel *p)
{
    u8 *r = sys->rdram, *hid = sys->rdramHidden;
    u32 idx = p->colorIndex;
    if (sys->jit)
    {
        u32 shift = st->colorFmt == FB_RGBA8888 ? 2 : (st->colorFmt == FB_I4 || st->colorFmt == FB_I8) ? 0 : 1;
        if (p->colorDirty) h64_jit_notify_write(sys, idx << shift, 4);
        if (p->depthDirty) h64_jit_notify_write(sys, p->depthIndex << 1, 2);
    }
    if (p->colorDirty)
    {
        switch (st->colorFmt)
        {
        case FB_I4:
            r[idx] = 0;
            if (idx & 1) hid[idx >> 1] = p->c[3];
            break;
        case FB_I8:
        {
            u8 col = (idx & 1) ? p->c[1] : p->c[0];
            r[idx] = col;
            if (idx & 1) hid[idx >> 1] = (u8)((col & 1) * 3);
            break;
        }
        case FB_RGBA5551:
        {
            u32 cov = (u32)p->c[3] >> 5;
            u32 word = ((u32)(p->c[0] & 0xF8) << 8) | ((u32)(p->c[1] & 0xF8) << 3) | ((u32)(p->c[2] & 0xF8) >> 2) | (cov >> 2);
            r[idx * 2] = (u8)(word >> 8); r[idx * 2 + 1] = (u8)word;
            hid[idx] = (u8)(cov & 3);
            break;
        }
        case FB_IA88:
            r[idx * 2] = p->c[0]; r[idx * 2 + 1] = p->c[3];
            hid[idx] = (u8)((p->c[3] & 1) * 3);
            break;
        default:
            r[idx * 4] = p->c[0]; r[idx * 4 + 1] = p->c[1]; r[idx * 4 + 2] = p->c[2]; r[idx * 4 + 3] = p->c[3];
            hid[(2 * idx) & (H64_RDRAM_SIZE / 2 - 1)] = (u8)((p->c[1] & 1) * 3);
            hid[(2 * idx + 1) & (H64_RDRAM_SIZE / 2 - 1)] = (u8)((p->c[3] & 1) * 3);
            break;
        }
    }
    if (p->depthDirty)
    {
        u32 word = ((u32)p->depth << 2) | ((u32)p->dz >> 2);
        idx = p->depthIndex;
        r[idx * 2] = (u8)(word >> 8); r[idx * 2 + 1] = (u8)word;
        hid[idx] = (u8)(p->dz & 3);
    }
}

static void write_color(H64RdpState *st, Pixel *p, u8 r, u8 g, u8 b, u8 a)
{
    p->c[0] = r; p->c[1] = g; p->c[2] = b;
    if (st->colorFmt != FB_I4) p->c[3] = a;
    p->colorDirty = 1;
}

static void fill_pixel(H64RdpState *st, Pixel *p)
{
    u32 col = st->fillColor;
    switch (st->colorFmt)
    {
    case FB_RGBA8888:
        write_color(st, p, (u8)(col >> 24), (u8)(col >> 16), (u8)(col >> 8), (u8)col);
        break;
    case FB_RGBA5551:
        col >>= ((p->colorIndex & 1) ^ 1) * 16;
        write_color(st, p, (u8)((col >> 8) & 0xF8), (u8)((col >> 3) & 0xF8), (u8)((col << 2) & 0xF8), (u8)((col & 1) * 0xE0));
        break;
    case FB_IA88:
        col >>= ((p->colorIndex & 1) ^ 1) * 16;
        write_color(st, p, (u8)(col >> 8), (u8)(col >> 8), (u8)(col >> 8), (u8)col);
        break;
    case FB_I8:
        col >>= ((p->colorIndex & 3) ^ 3) * 8;
        write_color(st, p, (u8)col, (u8)col, (u8)col, (u8)col);
        break;
    default:
        break;
    }
}

static void copy_pixel(H64RdpState *st, Pixel *p, u32 word)
{
    switch (st->colorFmt)
    {
    case FB_I4: p->c[0] = p->c[1] = p->c[2] = p->c[3] = 0; p->colorDirty = 1; break;
    case FB_I8: word &= 0xFF; write_color(st, p, (u8)word, (u8)word, (u8)word, (u8)word); break;
    case FB_RGBA5551:
        write_color(st, p, (u8)((word >> 8) & 0xF8), (u8)((word >> 3) & 0xF8), (u8)((word << 2) & 0xF8), (u8)((word & 1) * 0xE0));
        break;
    default: break;
    }
}

// ---- Span setup (span_setup.comp) ----
static s32 quantize_x(s32 x) { return (x >> 12) | ((x & 0xFFF) != 0 ? 1 : 0); }

static void span_setup(const H64RdpState *st, const TriSetup *ts, const AttrSetup *a, s32 y, Span *sp)
{
    int flip = (ts->flags & TS_FLIP) != 0, i;
    s32 dy = y - (ts->yh >> 2);
    s32 xh = ts->xh + dy * (ts->dxhdy << 2), baseX, xfrac;
    s32 drgbaDiff[4] = { 0, 0, 0, 0 }, dstzwDiff[4] = { 0, 0, 0, 0 };
    s32 yhBase = ts->yh & ~3, ymBase = ts->ym;
    s32 ylo = ts->yh > (s32)st->scissorYlo ? ts->yh : (s32)st->scissorYlo;
    s32 yhi = ts->yl < (s32)st->scissorYhi ? ts->yl : (s32)st->scissorYhi;
    s32 xleft[4], xright[4], lo = (s32)(st->scissorXlo << 1), hi = (s32)(st->scissorXhi << 1);
    int invalid[4], allOver = 1, allUnder = 1, allInvalid = 1;

    if (ts->flags & TS_DO_OFFSET)
    {
        xh += 3 * ts->dxhdy;
        for (i = 0; i < 4; i++)
        {
            s32 deh = a->drgbaDe[i] & ~0x1FF, dyh = a->drgbaDy[i] & ~0x1FF;
            drgbaDiff[i] = deh - (deh >> 2) - dyh + (dyh >> 2);
            deh = a->dstzwDe[i] & ~0x1FF; dyh = a->dstzwDy[i] & ~0x1FF;
            dstzwDiff[i] = deh - (deh >> 2) - dyh + (dyh >> 2);
        }
    }
    baseX = xh >> 15;
    xfrac = (ts->flags & TS_SKIP_XFRAC) ? 0 : ((xh >> 7) & 0xFF);
    for (i = 0; i < 4; i++)
    {
        s32 v = a->rgba[i] + dy * a->drgbaDe[i];
        sp->rgba[i] = ((v & ~0x1FF) + drgbaDiff[i] - xfrac * ((a->drgbaDx[i] >> 8) & ~1)) & ~0x3FF;
        v = a->stzw[i] + dy * a->dstzwDe[i];
        sp->stzw[i] = ((v & ~0x1FF) + dstzwDiff[i] - xfrac * ((a->dstzwDx[i] >> 8) & ~1)) & ~0x3FF;
    }
    sp->baseX = baseX;

    for (i = 0; i < 4; i++)
    {
        s32 ys = y * 4 + i;
        s32 xhs = ts->xh + (ys - yhBase) * ts->dxhdy;
        s32 xms = ts->xm + (ys - yhBase) * ts->dxmdy;
        s32 xls = ts->xl + (ys - ymBase) * ts->dxldy;
        int clipY = ys < ylo || ys >= yhi;
        if (ys < ts->ym) xls = xms;
        xls = h64_sext(xls, 27);
        xhs = h64_sext(xhs, 27);
        xhs = quantize_x(xhs);
        xls = quantize_x(xls);
        xleft[i] = flip ? xhs : xls;
        xright[i] = flip ? xls : xhs;
        invalid[i] = (xleft[i] >> 1) > (xright[i] >> 1);
        if ((xleft[i] < xright[i] ? xleft[i] : xright[i]) < hi) allOver = 0;
        if ((xleft[i] > xright[i] ? xleft[i] : xright[i]) >= lo) allUnder = 0;
        xleft[i] = h64_clamp(xleft[i], lo, hi);
        xright[i] = h64_clamp(xright[i], lo, hi);
        invalid[i] |= clipY;
        if (invalid[i]) { xleft[i] = 0xFFFF; xright[i] = 0; }
        else allInvalid = 0;
        sp->xleft[i] = (u16)xleft[i];
        sp->xright[i] = (u16)xright[i];
    }
    sp->startX = min4(sp->xleft) >> 3;
    sp->endX = max4(sp->xright) >> 3;
    sp->valid = !allInvalid && !allOver && !allUnder;
    if ((ts->flags & TS_INTERLACE_FIELD) && ((y & 1) != ((ts->flags & TS_INTERLACE_KEEP_ODD) ? 1 : 0)))
        sp->valid = 0;
    sp->lodLength = flip ? sp->endX - baseX : baseX - sp->startX;
}

// ---- Coverage (coverage.h) ----
static u32 compute_coverage(const Span *sp, s32 x)
{
    static const u16 off[4] = { 0, 4, 2, 6 };
    u32 cov = 0, i;
    for (i = 0; i < 8; i++)
    {
        u32 row = i >> 1;
        u16 xs = (u16)(off[(i & 1) | ((row & 1) << 1)] + ((u16)x << 3));
        int clip = xs < sp->xleft[row] || xs >= sp->xright[row];
        if (!clip) cov |= 1u << i;
    }
    return cov;
}

static s32 blend_coverage(s32 coverage, s32 memCoverage, int blendEn, int mode)
{
    switch (mode)
    {
    case 0: return blendEn ? (memCoverage + coverage < 7 ? memCoverage + coverage : 7) : ((coverage - 1) & 7);
    case 1: return (coverage + memCoverage) & 7;
    case 2: return 7;
    default: return memCoverage;
    }
}

// ---- Perspective (perspective.h) ----
static const s16 persp_table[64][2] = {
    { 0x4000, -252 * 4 }, { 0x3f04, -244 * 4 }, { 0x3e10, -238 * 4 }, { 0x3d22, -230 * 4 },
    { 0x3c3c, -223 * 4 }, { 0x3b5d, -218 * 4 }, { 0x3a83, -210 * 4 }, { 0x39b1, -205 * 4 },
    { 0x38e4, -200 * 4 }, { 0x381c, -194 * 4 }, { 0x375a, -189 * 4 }, { 0x369d, -184 * 4 },
    { 0x35e5, -179 * 4 }, { 0x3532, -175 * 4 }, { 0x3483, -170 * 4 }, { 0x33d9, -166 * 4 },
    { 0x3333, -162 * 4 }, { 0x3291, -157 * 4 }, { 0x31f4, -155 * 4 }, { 0x3159, -150 * 4 },
    { 0x30c3, -147 * 4 }, { 0x3030, -143 * 4 }, { 0x2fa1, -140 * 4 }, { 0x2f15, -137 * 4 },
    { 0x2e8c, -134 * 4 }, { 0x2e06, -131 * 4 }, { 0x2d83, -128 * 4 }, { 0x2d03, -125 * 4 },
    { 0x2c86, -123 * 4 }, { 0x2c0b, -120 * 4 }, { 0x2b93, -117 * 4 }, { 0x2b1e, -115 * 4 },
    { 0x2aab, -113 * 4 }, { 0x2a3a, -110 * 4 }, { 0x29cc, -108 * 4 }, { 0x2960, -106 * 4 },
    { 0x28f6, -104 * 4 }, { 0x288e, -102 * 4 }, { 0x2828, -100 * 4 }, { 0x27c4, -98 * 4 },
    { 0x2762, -96 * 4 }, { 0x2702, -94 * 4 }, { 0x26a4, -92 * 4 }, { 0x2648, -91 * 4 },
    { 0x25ed, -89 * 4 }, { 0x2594, -87 * 4 }, { 0x253d, -86 * 4 }, { 0x24e7, -85 * 4 },
    { 0x2492, -83 * 4 }, { 0x243f, -81 * 4 }, { 0x23ee, -80 * 4 }, { 0x239e, -79 * 4 },
    { 0x234f, -77 * 4 }, { 0x2302, -76 * 4 }, { 0x22b6, -74 * 4 }, { 0x226c, -74 * 4 },
    { 0x2222, -72 * 4 }, { 0x21da, -71 * 4 }, { 0x2193, -70 * 4 }, { 0x214d, -69 * 4 },
    { 0x2108, -67 * 4 }, { 0x20c5, -67 * 4 }, { 0x2082, -65 * 4 }, { 0x2041, -65 * 4 } };

static void perspective_divide(const s32 *stw, s32 *out, int *overflow)
{
    s32 w = stw[2], shift, normout, wnorm, rcp, mask, temp[2], prod[2], oob[2];
    int wCarry = w <= 0, i;
    w &= 0x7FFF;
    shift = 14 - h64_find_msb((u32)w);
    if (shift > 14) shift = 14;
    normout = (s32)(((u32)w << shift) & 0x3FFF);
    wnorm = normout & 0xFF;
    rcp = ((persp_table[normout >> 8][1] * wnorm) >> 10) + persp_table[normout >> 8][0];
    mask = ((1 << 30) - 1) & -((1 << 29) >> shift);
    for (i = 0; i < 2; i++)
    {
        prod[i] = stw[i] * rcp;
        oob[i] = prod[i] & mask;
        if (shift != 14) { prod[i] >>= 13 - shift; temp[i] = prod[i]; }
        else temp[i] = (s32)((u32)prod[i] << 1);
    }
    for (i = 0; i < 2; i++)
        if (oob[i] != mask && oob[i] != 0)
        {
            temp[i] = (prod[i] & (1 << 29)) == 0 ? 0x7FFF : -0x8000;
            *overflow = 1;
        }
    if (wCarry) { temp[0] = temp[1] = 0x7FFF; *overflow = 1; }
    out[0] = h64_clamp(temp[0], -0x10000, 0xFFFF);
    out[1] = h64_clamp(temp[1], -0x10000, 0xFFFF);
}

static void st_from_stw(const s32 *stw32, int perspective, s32 *st, int *overflow)
{
    s32 stw[3];
    stw[0] = stw32[0] >> 16; stw[1] = stw32[1] >> 16; stw[2] = stw32[2] >> 16;
    if (perspective) perspective_divide(stw, st, overflow);
    else { st[0] = stw[0]; st[1] = stw[1]; }
}

// ---- Combiner (combiner.h) ----
struct CombIn
{
    s32 shade[4], combined[4], texel0[4], texel1[4];
    s32 lodFrac, noise;
};

static void unpack_rgba(u32 c, s32 *o) { o[0] = (s32)(c >> 24); o[1] = (s32)((c >> 16) & 0xFF); o[2] = (s32)((c >> 8) & 0xFF); o[3] = (s32)(c & 0xFF); }

static s32 sel_rgb_muladd(const H64RdpState *st, const CombIn *in, u32 s, int ch)
{
    s32 c[4];
    switch (s)
    {
    case 0: return in->combined[ch];
    case 1: return in->texel0[ch];
    case 2: return in->texel1[ch];
    case 3: unpack_rgba(st->primColor, c); return c[ch];
    case 4: return in->shade[ch];
    case 5: unpack_rgba(st->envColor, c); return c[ch];
    case 6: return 0x100;
    case 7: return in->noise;
    default: return 0;
    }
}
static s32 sel_rgb_mulsub(const H64RdpState *st, const CombIn *in, u32 s, int ch)
{
    s32 c[4];
    switch (s)
    {
    case 0: return in->combined[ch];
    case 1: return in->texel0[ch];
    case 2: return in->texel1[ch];
    case 3: unpack_rgba(st->primColor, c); return c[ch];
    case 4: return in->shade[ch];
    case 5: unpack_rgba(st->envColor, c); return c[ch];
    case 6: return (s32)st->keyCenter[ch];
    case 7: return st->convert[4];   // K4, 9 bits
    default: return 0;
    }
}
static s32 sel_rgb_mul(const H64RdpState *st, const CombIn *in, u32 s, int ch)
{
    s32 c[4];
    switch (s)
    {
    case 0: return in->combined[ch];
    case 1: return in->texel0[ch];
    case 2: return in->texel1[ch];
    case 3: unpack_rgba(st->primColor, c); return c[ch];
    case 4: return in->shade[ch];
    case 5: unpack_rgba(st->envColor, c); return c[ch];
    case 6: return (s32)st->keyScale[ch];
    case 7: return in->combined[3];
    case 8: return in->texel0[3];
    case 9: return in->texel1[3];
    case 10: return (s32)(st->primColor & 0xFF);
    case 11: return in->shade[3];
    case 12: return (s32)(st->envColor & 0xFF);
    case 13: return in->lodFrac;
    case 14: return st->primLodFrac;
    case 15: return st->convert[5];   // K5, 9 bits
    default: return 0;
    }
}
static s32 sel_rgb_add(const H64RdpState *st, const CombIn *in, u32 s, int ch)
{
    return s == 7 ? 0 : sel_rgb_muladd(st, in, s, ch);   // same inputs, 6 = one, 7 = zero
}
static s32 sel_alpha_addsub(const H64RdpState *st, const CombIn *in, u32 s)
{
    switch (s)
    {
    case 0: return in->combined[3];
    case 1: return in->texel0[3];
    case 2: return in->texel1[3];
    case 3: return (s32)(st->primColor & 0xFF);
    case 4: return in->shade[3];
    case 5: return (s32)(st->envColor & 0xFF);
    case 6: return 0x100;
    default: return 0;
    }
}
static s32 sel_alpha_mul(const H64RdpState *st, const CombIn *in, u32 s)
{
    switch (s)
    {
    case 0: return in->lodFrac;
    case 1: return in->texel0[3];
    case 2: return in->texel1[3];
    case 3: return (s32)(st->primColor & 0xFF);
    case 4: return in->shade[3];
    case 5: return (s32)(st->envColor & 0xFF);
    case 6: return st->primLodFrac;
    default: return 0;
    }
}

static s32 special_expand(s32 v) { return h64_sext(v - 0x80, 9) + 0x80; }

static void combiner_equation(const H64RdpState *st, const CombIn *in, const H64RdpCombiner *c, s32 *out)
{
    int ch;
    for (ch = 0; ch < 4; ch++)
    {
        s32 a, b, m, d, color;
        if (ch < 3)
        {
            a = sel_rgb_muladd(st, in, c->rgbMulAdd, ch);
            b = sel_rgb_mulsub(st, in, c->rgbMulSub, ch);
            m = sel_rgb_mul(st, in, c->rgbMul, ch);
            d = sel_rgb_add(st, in, c->rgbAdd, ch);
        }
        else
        {
            a = sel_alpha_addsub(st, in, c->aMulAdd);
            b = sel_alpha_addsub(st, in, c->aMulSub);
            m = sel_alpha_mul(st, in, c->aMul);
            d = sel_alpha_addsub(st, in, c->aAdd);
        }
        m = h64_sext(m, 9);
        a = special_expand(a);
        b = special_expand(b);
        d = special_expand(d);
        color = (a - b) * m + 0x80;
        out[ch] = (s16)((s16)(color >> 8) + (s16)d);
    }
}

static void combiner_cycle0(const H64RdpState *st, const CombIn *in, const H64RdpCombiner *c, s32 alphaDith,
                            s32 coverage, int cvgTimesAlpha, int alphaCvgSelect, int alphaTest, s32 *out, s32 *alphaRef)
{
    combiner_equation(st, in, c, out);
    if (alphaTest)
    {
        s32 ca = clamp9(out[3]);
        s32 ea = ca + ((ca + 1) >> 8);
        if (alphaCvgSelect) ea = cvgTimesAlpha ? (ea * coverage + 4) >> 3 : coverage << 5;
        else ea += alphaDith;
        *alphaRef = h64_clamp(ea, 0, 0xFF);
    }
    else
        *alphaRef = 0;
}

static void combiner_cycle1(const H64RdpState *st, const CombIn *in, const H64RdpCombiner *c, s32 alphaDith,
                            s32 *coverage, int cvgTimesAlpha, int alphaCvgSelect, s32 *out)
{
    s32 ea, ma;
    int ch;
    combiner_equation(st, in, c, out);
    for (ch = 0; ch < 4; ch++) out[ch] = clamp9(out[ch]);   // clamp_9bit_notrunc
    ea = out[3] + ((out[3] + 1) >> 8);
    if (cvgTimesAlpha) { ma = (ea * *coverage + 4) >> 3; *coverage = ma >> 5; }
    else ma = *coverage << 5;
    if (alphaCvgSelect) ea = ma;
    else ea += alphaDith;
    out[3] = h64_clamp(ea, 0, 0xFF);
}

// ---- Dither (dither.h) ----
static const u8 dither_matrices[2][16] = {
    { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 },
    { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 } };

static void dither_coefficients(const H64RdpState *st, s32 x, s32 y, int modeRgb, int modeAlpha, s32 *rgbDith, s32 *alphaDith)
{
    const s32 SPLAT = (1 << 0) | (1 << 3) | (1 << 6);
    if (modeRgb < 2) *rgbDith = dither_matrices[modeRgb][(y & 3) * 4 + (x & 3)] * SPLAT;
    else if (modeRgb == 2) *rgbDith = st->noise & 0x1FF;
    else *rgbDith = 0;
    if (modeAlpha == 3) *alphaDith = 0;
    else if (modeAlpha == 2) *alphaDith = st->noise & 7;
    else
    {
        *alphaDith = modeRgb >= 2 ? dither_matrices[modeRgb & 1][(y & 3) * 4 + (x & 3)] : (*rgbDith & 7);
        if (modeAlpha == 1) *alphaDith = ~*alphaDith & 7;
    }
}

static void rgb_dither(u8 *rgb, s32 dith)
{
    int i;
    for (i = 0; i < 3; i++)
    {
        s32 d = (dith >> (3 * i)) & 7, o = rgb[i];
        s32 r = o > 247 ? 255 : (o & 0xF8) + 8;
        s32 replace = (d - (o & 7)) >> 31;
        rgb[i] = (u8)((o + ((r - o) & replace)) & 0xFF);
    }
}

// ---- Depth test (depth_test.h) ----
static s32 combine_dz(s32 dz) { return dz ? 1 << h64_find_msb((u32)dz) : 0; }

static int depth_test(const H64RdpState *st, s32 z, s32 dz, s32 dzCompressed, u32 curDepth, u32 curDz,
                      s32 *coverageCount, s32 curCoverage, int *blendEn, int *coverageWrap, s32 *shift)
{
    int forceBlend = (st->depthBlendFlags & DB_FORCE_BLEND) != 0, aa = (st->depthBlendFlags & DB_AA) != 0;
    int pass;
    if (st->depthBlendFlags & DB_DEPTH_TEST)
    {
        s32 memZ = z_decompress(curDepth), memDz = 1 << curDz, precision = (s32)((curDepth >> 11) & 0xF);
        int coplanar = 0, farther, overflow, maxZ, front, nearer;
        s32 combined, combinedInter;
        shift[0] = h64_clamp(dzCompressed - (s32)curDz, 0, 4);
        shift[1] = h64_clamp((s32)curDz - dzCompressed, 0, 4);
        if (precision < 3)
        {
            if (memDz != 0x8000) { memDz <<= 1; if (memDz < (16 >> precision)) memDz = 16 >> precision; }
            else { coplanar = 1; memDz = 0xFFFF; }
        }
        combined = combine_dz(dz | memDz);
        combinedInter = combined;
        combined <<= 3;
        farther = coplanar || (z + combined) >= memZ;
        overflow = (*coverageCount + curCoverage) >= 8;
        *blendEn = forceBlend || (!overflow && aa && farther);
        *coverageWrap = overflow;
        maxZ = memZ == 0x3FFFF;
        front = z < memZ;
        nearer = coplanar || (z - combined) <= memZ;
        switch (st->zMode)
        {
        case 0: pass = maxZ || (overflow ? front : nearer); break;
        case 1:
            if (!front || !farther || !overflow) pass = maxZ || (overflow ? front : nearer);
            else
            {
                s32 sh = dz_compress_msb(combinedInter & 0xFFFF);
                s32 coeff = ((memZ >> sh) - (z >> sh)) & 0xF;
                s32 c = (coeff * *coverageCount) >> 3;
                *coverageCount = c < 8 ? c : 8;
                pass = 1;
            }
            break;
        case 2: pass = front || maxZ; break;
        default: pass = farther && nearer && !maxZ; break;
        }
    }
    else
    {
        int overflow = (*coverageCount + curCoverage) >= 8;
        shift[0] = 0;
        shift[1] = 0xF - dzCompressed < 4 ? 0xF - dzCompressed : 4;
        *blendEn = forceBlend || (!overflow && aa);
        *coverageWrap = overflow;
        pass = 1;
    }
    return pass;
}

// ---- Blender (blender.h) ----
static void blender(const H64RdpState *st, const u8 *pixel, const u8 *memory, u8 shadeAlpha, const u8 *modes,
                    int blendEn, int coverageWrap, const s32 *shift, int finalCycle, u8 *out)
{
    u8 fog[4], blend[4], rgb0[3], rgb1[3];
    s32 a0, a1;
    int forceBlend = (st->depthBlendFlags & DB_FORCE_BLEND) != 0, i;
    fog[0] = (u8)(st->fogColor >> 24); fog[1] = (u8)(st->fogColor >> 16); fog[2] = (u8)(st->fogColor >> 8); fog[3] = (u8)st->fogColor;
    blend[0] = (u8)(st->blendColor >> 24); blend[1] = (u8)(st->blendColor >> 16); blend[2] = (u8)(st->blendColor >> 8); blend[3] = (u8)st->blendColor;
    for (i = 0; i < 3; i++)
    {
        const u8 *src2 = modes[2] == 0 ? pixel : modes[2] == 1 ? memory : modes[2] == 2 ? blend : fog;
        const u8 *src1 = modes[0] == 0 ? pixel : modes[0] == 1 ? memory : modes[0] == 2 ? blend : fog;
        rgb1[i] = src2[i];
        rgb0[i] = src1[i];
    }
    if (finalCycle && (st->depthBlendFlags & DB_COLOR_ON_CVG) && !coverageWrap)
    {
        memcpy(out, rgb1, 3);
        return;
    }
    if (finalCycle && (!blendEn || (modes[1] == 0 && modes[3] == 0 && pixel[3] == 0xFF)))
    {
        memcpy(out, rgb0, 3);
        return;
    }
    switch (modes[1])
    {
    case 0: a0 = pixel[3]; break;
    case 1: a0 = fog[3]; break;
    case 2: a0 = shadeAlpha; break;
    default: a0 = 0; break;
    }
    switch (modes[3])
    {
    case 0: a1 = ~a0 & 0xFF; break;
    case 1: a1 = memory[3]; break;
    case 2: a1 = 0xFF; break;
    default: a1 = 0; break;
    }
    a0 >>= 3;
    a1 >>= 3;
    if (modes[3] == 1)
    {
        a0 = (a0 >> shift[0]) & 0x3C;
        a1 = (a1 >> shift[1]) | 3;
    }
    for (i = 0; i < 3; i++)
    {
        s16 blended = (s16)(rgb0[i] * a0 + rgb1[i] * (a1 + 1));
        if (!finalCycle || forceBlend) out[i] = (u8)((blended >> 5) & 0xFF);
        else
        {
            s32 sum = (a0 >> 2) + (a1 >> 2) + 1;
            s32 b = (blended >> 2) & 0x7FF;
            out[i] = h64_rdp_blender_lut[((sum << 11) | b) & 0x7FFF];
        }
    }
}

// ---- Pixel pipeline ----
struct PrimContext
{
    TriSetup ts;
    AttrSetup a;
    Derived d;
};

static void depth_blend(H64RdpState *st, const Derived *d, Pixel *p, const s32 *combined, s32 z, s32 dith,
                        s32 coverageCount, u8 shadeAlpha)
{
    int imageRead = (st->depthBlendFlags & DB_IMAGE_READ) != 0, blendEn = 0, coverageWrap = 0;
    u8 memory[4], pixel[4], rgb[3];
    s32 shift[2], memCov, newCov;
    switch (st->colorFmt)
    {
    case FB_I4: memory[0] = memory[1] = memory[2] = 0; memory[3] = 0xE0; break;
    case FB_I8: memory[0] = memory[1] = memory[2] = p->c[0]; memory[3] = 0xE0; break;
    case FB_RGBA5551:
        memory[0] = (u8)(p->c[0] & 0xF8); memory[1] = (u8)(p->c[1] & 0xF8); memory[2] = (u8)(p->c[2] & 0xF8);
        memory[3] = imageRead ? (u8)(p->c[3] & 0xE0) : 0xE0;
        break;
    case FB_IA88:
        memory[0] = memory[1] = memory[2] = p->c[0];
        memory[3] = imageRead ? (u8)(p->c[3] & 0xE0) : 0xE0;
        break;
    default:
        memory[0] = p->c[0]; memory[1] = p->c[1]; memory[2] = p->c[2];
        memory[3] = imageRead ? (u8)(p->c[3] & 0xE0) : 0xE0;
        break;
    }
    memCov = memory[3] >> 5;
    if (!depth_test(st, z, d->dz, d->dzCompressed, p->depth, p->dz, &coverageCount, memCov, &blendEn, &coverageWrap, shift))
        return;
    if ((st->depthBlendFlags & DB_AA) && coverageCount == 0)
        return;
    pixel[0] = (u8)combined[0]; pixel[1] = (u8)combined[1]; pixel[2] = (u8)combined[2]; pixel[3] = (u8)combined[3];
    if (st->depthBlendFlags & DB_MULTI_CYCLE)
    {
        blender(st, pixel, memory, shadeAlpha, st->blend[0], blendEn, coverageWrap, shift, 0, rgb);
        pixel[0] = rgb[0]; pixel[1] = rgb[1]; pixel[2] = rgb[2];
        blender(st, pixel, memory, shadeAlpha, st->blend[1], blendEn, coverageWrap, shift, 1, rgb);
    }
    else
        blender(st, pixel, memory, shadeAlpha, st->blend[0], blendEn, coverageWrap, shift, 1, rgb);
    if (st->depthBlendFlags & DB_DITHER) rgb_dither(rgb, dith);
    newCov = blend_coverage(coverageCount, memCov, blendEn, st->coverageMode);
    write_color(st, p, rgb[0], rgb[1], rgb[2], (u8)(newCov << 5));
    if (st->depthBlendFlags & DB_DEPTH_UPDATE)
    {
        p->depth = z_compress(z);
        p->dz = (u8)d->dzCompressed;
        p->depthDirty = 1;
    }
}

static void sample_tile(H64RdpState *st, u32 tile, const s32 *stc, int sampleQuad, int convertOne, int bilerp,
                        const s32 *factors, const H64RdpTexel *prev, H64RdpTexel *out)
{
    u32 f = st->rasterFlags;
    h64_rdp_sample(st, &st->tiles[tile & 7], stc, (f & RS_TLUT) != 0, (f & RS_TLUT_TYPE) != 0, sampleQuad,
                   (f & RS_MID_TEXEL) != 0, convertOne, bilerp, factors, prev, out);
}

static void shade_and_write(H64System *sys, H64RdpState *st, PrimContext *pc, const Span *sp, const Span *next, s32 x, s32 y)
{
    const TriSetup *ts = &pc->ts;
    const AttrSetup *a = &pc->a;
    const Derived *d = &pc->d;
    u32 f = d->flags;
    int flip = (ts->flags & TS_FLIP) != 0, perspective = (f & RS_PERSPECTIVE) != 0;
    Pixel p;

    if (d->needNoise) reseed_noise(st, (u32)x, (u32)y, st->primitives);

    if (f & RS_COPY)
    {
        s32 dx, sOffset, lerpDx, stw[3], stc[2], texel;
        int ovf = 0, fbSize = fb_size_of(st->colorFmt), dxShift = fbSize == 1 ? 3 : fbSize == 2 ? 2 : fbSize == 4 ? 1 : 0;
        s32 dxMask = fbSize == 1 ? ~7 : fbSize == 2 ? ~3 : 0;
        if (x < sp->startX || x > sp->endX) return;
        dx = flip ? x - sp->startX : sp->endX - x;
        sOffset = dx - (dx & dxMask);
        lerpDx = (dx >> dxShift) * (flip ? 1 : -1);
        stw[0] = sp->stzw[0] + (a->dstzwDx[0] & ~0x1F) * lerpDx;
        stw[1] = sp->stzw[1] + (a->dstzwDx[1] & ~0x1F) * lerpDx;
        stw[2] = sp->stzw[3] + (a->dstzwDx[3] & ~0x1F) * lerpDx;
        st_from_stw(stw, perspective, stc, &ovf);
        texel = h64_rdp_sample_copy(st, &st->tiles[ts->tile & 7], stc[0], stc[1], sOffset, (f & RS_TLUT) != 0, fbSize);
        if ((f & RS_ALPHA_TEST) && fbSize == 2 && (texel & 1) == 0) return;
        load_pixel(sys, st, (u32)x, (u32)y, &p);
        copy_pixel(st, &p, (u32)texel);
        store_pixel(sys, st, &p);
        return;
    }
    if (f & RS_FILL)
    {
        if (x < sp->startX || x > sp->endX) return;
        load_pixel(sys, st, (u32)x, (u32)y, &p);
        fill_pixel(st, &p);
        store_pixel(sys, st, &p);
        return;
    }

    {
        u32 coverage = compute_coverage(sp, x), tile0, tile1, maxLevel;
        s32 coverageCount, dx = x - sp->baseX, dir = flip ? 1 : -1, z, shade[4], fc, xoff, yoff, i;
        s32 stw[3], stwDx[3], stwDy[3], stc[2], stDx[2], stDy[2], lodFrac = 0, rgbDith, alphaDith;
        s32 combined[4], alphaRef = 0;
        int ovf = 0, multi = (f & RS_MULTI_CYCLE) != 0, aa = (f & RS_AA) != 0;
        H64RdpTexel t0, t1;
        CombIn in;
        u8 shadeAlpha;
        if (!coverage) return;
        coverageCount = 0;
        for (i = 0; i < 8; i++) coverageCount += (coverage >> i) & 1;
        if (!aa && !(coverage & 1)) return;

        // Shade (interpolate_rgba)
        fc = h64_find_lsb(coverage);
        yoff = fc >> 1;
        xoff = ((fc & 1) << 1) + (yoff & 1);
        for (i = 0; i < 4; i++)
        {
            s32 v = sp->rgba[i] + (a->drgbaDx[i] & ~0x1F) * dx;
            s16 sn = (s16)(v >> 14);
            sn = (s16)(sn << 2);
            sn = (s16)(sn + xoff * (s16)(a->drgbaDx[i] >> 14) + yoff * (s16)(a->drgbaDy[i] >> 14));
            sn = (s16)(sn >> 4);
            shade[i] = clamp9(sn);
        }

        // S, T, Z (interpolate_stz)
        stw[0] = sp->stzw[0] + (a->dstzwDx[0] & ~0x1F) * dx;
        stw[1] = sp->stzw[1] + (a->dstzwDx[1] & ~0x1F) * dx;
        stw[2] = sp->stzw[3] + (a->dstzwDx[3] & ~0x1F) * dx;
        st_from_stw(stw, perspective, stc, &ovf);
        if (d->usesLod)
        {
            stwDx[0] = stw[0] + dir * (a->dstzwDx[0] & ~0x1F);
            stwDx[1] = stw[1] + dir * (a->dstzwDx[1] & ~0x1F);
            stwDx[2] = stw[2] + dir * (a->dstzwDx[3] & ~0x1F);
            stwDy[0] = stw[0] + (a->dstzwDy[0] & ~0x7FFF);
            stwDy[1] = stw[1] + (a->dstzwDy[1] & ~0x7FFF);
            stwDy[2] = stw[2] + (a->dstzwDy[3] & ~0x7FFF);
            st_from_stw(stwDx, perspective, stDx, &ovf);
            st_from_stw(stwDy, perspective, stDy, &ovf);
        }
        z = sp->stzw[2] + a->dstzwDx[2] * dx;
        {
            s32 sz = z >> 10;
            sz <<= 2;
            sz += xoff * (a->dstzwDx[2] >> 10) + yoff * (a->dstzwDy[2] >> 10);
            sz >>= 5;
            z = clamp_z(sz);
        }

        // Textures
        tile0 = ts->tile & 7;
        tile1 = (tile0 + 1) & 7;
        maxLevel = ts->tile >> 3;
        if (d->usesLod)
            h64_rdp_compute_lod(&tile0, &tile1, &lodFrac, maxLevel, d->minLod, stc, stDx, stDy, ovf,
                                (f & RS_TEX_LOD) != 0, (f & RS_SHARPEN_LOD) != 0, (f & RS_DETAIL_LOD) != 0);
        memset(&t0, 0, sizeof(t0));
        memset(&t1, 0, sizeof(t1));
        if (d->usesTexel0)
            sample_tile(st, tile0, stc, (f & RS_SAMPLE_QUAD) != 0, 0, (f & RS_BILERP0) != 0, d->factors, &t0, &t0);
        {
            int usesTexel1 = d->usesTexel1;
            s32 st1[2];
            st1[0] = stc[0]; st1[1] = stc[1];
            if (d->usesPipelinedTexel1)
            {
                int longSpan = sp->lodLength >= 8, endSpan = x == (flip ? sp->endX : sp->startX);
                if (endSpan && longSpan && next && next->valid)
                {
                    s32 nstw[3];
                    nstw[0] = next->stzw[0]; nstw[1] = next->stzw[1]; nstw[2] = next->stzw[3];
                    st_from_stw(nstw, perspective, st1, &ovf);
                }
                else
                {
                    s32 nstw[3];
                    nstw[0] = sp->stzw[0] + (a->dstzwDx[0] & ~0x1F) * (dx + dir);
                    nstw[1] = sp->stzw[1] + (a->dstzwDx[1] & ~0x1F) * (dx + dir);
                    nstw[2] = sp->stzw[3] + (a->dstzwDx[3] & ~0x1F) * (dx + dir);
                    st_from_stw(nstw, perspective, st1, &ovf);
                }
                tile1 = tile0;
                usesTexel1 = 1;
            }
            if (usesTexel1)
            {
                if ((f & RS_CONVERT_ONE) && !(f & RS_BILERP1))
                    h64_rdp_texture_convert(&t0, d->factors, &t1);
                else
                    sample_tile(st, tile1, st1, (f & RS_SAMPLE_QUAD) != 0, (f & RS_CONVERT_ONE) != 0,
                                (f & RS_BILERP1) != 0, d->factors, &t0, &t1);
            }
        }

        dither_coefficients(st, x, y >> ((f & RS_INTERLACE_FIELD) ? 1 : 0), (int)(st->dither >> 2), (int)(st->dither & 3),
                            &rgbDith, &alphaDith);

        memcpy(in.shade, shade, sizeof(shade));
        memset(in.combined, 0, sizeof(in.combined));
        memcpy(in.texel0, t0.c, sizeof(in.texel0));
        memcpy(in.texel1, t1.c, sizeof(in.texel1));
        in.lodFrac = lodFrac;
        in.noise = (s32)(((st->noise & 7) << 6) | 0x20);
        if (multi)
        {
            s32 c0[4];
            combiner_cycle0(st, &in, &st->combiner[0], alphaDith, coverageCount, (f & RS_CVG_TIMES_ALPHA) != 0,
                            (f & RS_ALPHA_CVG_SELECT) != 0, (f & RS_ALPHA_TEST) != 0, c0, &alphaRef);
            memcpy(in.combined, c0, sizeof(c0));
            // Pipelining: texel1 is promoted to texel0 in the second cycle.
            memcpy(in.texel0, t1.c, sizeof(in.texel0));
            memcpy(in.texel1, t0.c, sizeof(in.texel1));
            if (d->needNoiseDual)
            {
                reseed_noise(st, (u32)x + 1023, (u32)y + 7, st->primitives + 11);
                in.noise = (s32)(((st->noise & 7) << 6) | 0x20);
            }
            combiner_cycle1(st, &in, &st->combiner[1], alphaDith, &coverageCount, (f & RS_CVG_TIMES_ALPHA) != 0,
                            (f & RS_ALPHA_CVG_SELECT) != 0, combined);
        }
        else
        {
            combiner_cycle1(st, &in, &st->combiner[1], alphaDith, &coverageCount, (f & RS_CVG_TIMES_ALPHA) != 0,
                            (f & RS_ALPHA_CVG_SELECT) != 0, combined);
            alphaRef = combined[3];
        }
        if (aa && coverageCount == 0) return;
        if (f & RS_ALPHA_TEST)
        {
            s32 threshold = (f & RS_ALPHA_TEST_DITHER) ? (s32)(st->noise & 0xFF) : (s32)(st->blendColor & 0xFF);
            if (alphaRef < threshold) return;
        }
        shadeAlpha = (u8)(shade[3] + alphaDith < 0xFF ? shade[3] + alphaDith : 0xFF);
        load_pixel(sys, st, (u32)x, (u32)y, &p);
        if (st->probeOn && (u32)x == st->probeX && (u32)y == st->probeY)
        {
            const H64RdpCombiner *c0 = &st->combiner[0], *c1 = &st->combiner[1];
            u8 before[4];
            memcpy(before, p.c, 4);
            depth_blend(st, d, &p, combined, z, rgbDith, coverageCount, shadeAlpha);
            H64_INFO("[probe] prim %u at %d,%d: comb0 rgb(%u %u %u %u) a(%u %u %u %u) comb1 rgb(%u %u %u %u) a(%u %u %u %u) "
                     "blend %u%u%u%u/%u%u%u%u raster %08X db %08X cvg %u zmode %u prim %08X env %08X fog %08X blendc %08X "
                     "tile %u fmt %u size %u | combined %d %d %d %d shadeA %u cvgcount %d z %d | %02X%02X%02X%02X -> %02X%02X%02X%02X",
                     st->primitives, x, y, c0->rgbMulAdd, c0->rgbMulSub, c0->rgbMul, c0->rgbAdd, c0->aMulAdd, c0->aMulSub, c0->aMul,
                     c0->aAdd, c1->rgbMulAdd, c1->rgbMulSub, c1->rgbMul, c1->rgbAdd, c1->aMulAdd, c1->aMulSub, c1->aMul, c1->aAdd,
                     st->blend[0][0], st->blend[0][1], st->blend[0][2], st->blend[0][3], st->blend[1][0], st->blend[1][1],
                     st->blend[1][2], st->blend[1][3], st->rasterFlags, st->depthBlendFlags, st->coverageMode, st->zMode,
                     st->primColor, st->envColor, st->fogColor, st->blendColor, pc->ts.tile & 7, st->tiles[pc->ts.tile & 7].fmt,
                     st->tiles[pc->ts.tile & 7].size, combined[0], combined[1], combined[2], combined[3], shadeAlpha, coverageCount,
                     z, before[0], before[1], before[2], before[3], p.c[0], p.c[1], p.c[2], p.c[3]);
        }
        else
            depth_blend(st, d, &p, combined, z, rgbDith, coverageCount, shadeAlpha);
        store_pixel(sys, st, &p);
    }
}

// ---- Derived state (rdp_renderer.cpp) ----
static int comb_texel0(const H64RdpCombiner *c)
{
    return c->rgbMulAdd == 1 || c->rgbMulSub == 1 || c->rgbMul == 1 || c->rgbAdd == 1 || c->rgbMul == 8 ||
           c->aMulAdd == 1 || c->aMulSub == 1 || c->aMul == 1 || c->aAdd == 1;
}
static int comb_texel1(const H64RdpCombiner *c)
{
    return c->rgbMulAdd == 2 || c->rgbMulSub == 2 || c->rgbMul == 2 || c->rgbAdd == 2 || c->rgbMul == 9 ||
           c->aMulAdd == 2 || c->aMulSub == 2 || c->aMul == 2 || c->aAdd == 2;
}
static int comb_lod(const H64RdpCombiner *c) { return c->rgbMul == 13 || c->aMul == 0; }

static void derive(H64RdpState *st, const AttrSetup *a, Derived *d)
{
    u32 f = st->rasterFlags;
    int multi = (f & RS_MULTI_CYCLE) != 0, i;
    if (st->usePrimDepth)
    {
        d->dz = (s32)st->primDz;
        d->dzCompressed = dz_compress_pot(d->dz);
    }
    else
    {
        s32 dzdx = a->dstzwDx[2] >> 16, dzdy = a->dstzwDy[2] >> 16;
        s32 dzpix = (dzdx < 0 ? (~dzdx & 0x7FFF) : dzdx) + (dzdy < 0 ? (~dzdy & 0x7FFF) : dzdy);
        dzpix = normalize_dzpix(dzpix);
        d->dz = dzpix;
        d->dzCompressed = dz_compress_pot(dzpix);
    }
    for (i = 0; i < 4; i++) d->factors[i] = st->convert[i];
    d->minLod = st->primMinLevel;
    if (multi)
    {
        d->usesTexel0 = comb_texel0(&st->combiner[0]) || comb_texel1(&st->combiner[1]);
        d->usesTexel1 = comb_texel1(&st->combiner[0]) || comb_texel0(&st->combiner[1]);
        d->usesPipelinedTexel1 = 0;
        d->usesLod = comb_lod(&st->combiner[0]) || comb_lod(&st->combiner[1]);
    }
    else
    {
        d->usesTexel0 = comb_texel0(&st->combiner[1]);
        d->usesTexel1 = 0;
        d->usesPipelinedTexel1 = comb_texel1(&st->combiner[1]);
        d->usesLod = 0;
    }
    if (d->usesTexel1 && (f & RS_CONVERT_ONE)) d->usesTexel0 = 1;
    if (f & RS_TEX_LOD) d->usesLod = 1;
    // Noise (deduce_noise_state)
    d->needNoise = d->needNoiseDual = 0;
    if ((st->dither & 3) == 2 || ((st->dither >> 2) & 3) == 2) d->needNoise = 1;
    else if (!(f & (RS_COPY | RS_FILL)))
    {
        if (multi && st->combiner[0].rgbMulAdd == 7) d->needNoise = 1;
        if (st->combiner[1].rgbMulAdd == 7) d->needNoise = 1;
        if (multi && st->combiner[0].rgbMulAdd == 7 && st->combiner[1].rgbMulAdd == 7) d->needNoiseDual = 1;
        if ((f & (RS_ALPHA_TEST | RS_ALPHA_TEST_DITHER)) == (RS_ALPHA_TEST | RS_ALPHA_TEST_DITHER)) d->needNoise = 1;
    }
    // normalize_static_state
    if (f & RS_FILL) f = RS_FILL;
    else if (f & RS_COPY)
        f &= RS_COPY | RS_TLUT | RS_TLUT_TYPE | RS_TEX_LOD | RS_DETAIL_LOD | RS_PERSPECTIVE | RS_ALPHA_TEST;
    else if (!(f & RS_MULTI_CYCLE) && !d->usesPipelinedTexel1)
        f &= ~(RS_BILERP1 | RS_CONVERT_ONE);
    d->flags = f;
}

static void draw(H64System *sys, H64RdpState *st, TriSetup *ts, AttrSetup *a)
{
    PrimContext pc;
    s32 minLine, maxLine, y, fbWidth = (s32)st->colorWidth;
    Span cur, next;
    // fixup_triangle_setup
    if (ts->ym < (ts->yh & ~3)) ts->ym = 0x7FFF;
    if (st->rasterFlags & RS_INTERLACE_FIELD)
    {
        ts->flags |= TS_INTERLACE_FIELD;
        if (st->rasterFlags & RS_INTERLACE_KEEP_ODD) ts->flags |= TS_INTERLACE_KEEP_ODD;
    }
    if (st->usePrimDepth)
    {
        a->stzw[2] = st->primDepth;
        a->dstzwDx[2] = a->dstzwDe[2] = a->dstzwDy[2] = 0;
    }
    pc.ts = *ts;
    pc.a = *a;
    derive(st, a, &pc.d);
    st->primitives++;

    minLine = (ts->yh > (s32)st->scissorYlo ? ts->yh : (s32)st->scissorYlo) >> 2;
    maxLine = ((ts->yl - 1) < (s32)st->scissorYhi - 1 ? (ts->yl - 1) : (s32)st->scissorYhi - 1) >> 2;
    if (maxLine < minLine) return;
    if (maxLine - minLine > 1023) maxLine = minLine + 1023;

    span_setup(st, &pc.ts, &pc.a, minLine, &next);
    for (y = minLine; y <= maxLine; y++)
    {
        s32 x, x0, x1;
        cur = next;
        span_setup(st, &pc.ts, &pc.a, y + 1, &next);
        if (!cur.valid) continue;
        x0 = cur.startX < 0 ? 0 : cur.startX;
        x1 = cur.endX >= fbWidth ? fbWidth - 1 : cur.endX;
        for (x = x0; x <= x1; x++)
            shade_and_write(sys, st, &pc, &cur, &next, x, y);
    }
}

// ---- Command decoding (rdp_device.cpp) ----
static void decode_triangle(H64RdpState *st, const u32 *w, TriSetup *ts)
{
    int copy = (st->rasterFlags & RS_COPY) != 0;
    int flip = (w[0] & 0x800000u) != 0, signDxhdy = (w[5] & 0x80000000u) != 0;
    memset(ts, 0, sizeof(*ts));
    ts->flags = (flip ? TS_FLIP : 0) | (flip == signDxhdy ? TS_DO_OFFSET : 0) | (copy ? TS_SKIP_XFRAC : 0);
    ts->tile = (w[0] >> 16) & 63;
    ts->yl = h64_sext((s32)w[0], 14);
    ts->ym = h64_sext((s32)(w[1] >> 16), 14);
    ts->yh = h64_sext((s32)w[1], 14);
    ts->xl = h64_sext((s32)w[2], 28) >> 1;
    ts->xh = h64_sext((s32)w[4], 28) >> 1;
    ts->xm = h64_sext((s32)w[6], 28) >> 1;
    ts->dxldy = h64_sext((s32)(w[3] >> 2), 28) >> 1;
    ts->dxhdy = h64_sext((s32)(w[5] >> 2), 28) >> 1;
    ts->dxmdy = h64_sext((s32)(w[7] >> 2), 28) >> 1;
}

static void decode_rgba(const u32 *w, AttrSetup *a)
{
    a->rgba[0] = (s32)((w[0] & 0xFFFF0000u) | ((w[4] >> 16) & 0xFFFF));
    a->rgba[1] = (s32)((w[0] << 16) | (w[4] & 0xFFFF));
    a->rgba[2] = (s32)((w[1] & 0xFFFF0000u) | ((w[5] >> 16) & 0xFFFF));
    a->rgba[3] = (s32)((w[1] << 16) | (w[5] & 0xFFFF));
    a->drgbaDx[0] = (s32)((w[2] & 0xFFFF0000u) | ((w[6] >> 16) & 0xFFFF));
    a->drgbaDx[1] = (s32)((w[2] << 16) | (w[6] & 0xFFFF));
    a->drgbaDx[2] = (s32)((w[3] & 0xFFFF0000u) | ((w[7] >> 16) & 0xFFFF));
    a->drgbaDx[3] = (s32)((w[3] << 16) | (w[7] & 0xFFFF));
    a->drgbaDe[0] = (s32)((w[8] & 0xFFFF0000u) | ((w[12] >> 16) & 0xFFFF));
    a->drgbaDe[1] = (s32)((w[8] << 16) | (w[12] & 0xFFFF));
    a->drgbaDe[2] = (s32)((w[9] & 0xFFFF0000u) | ((w[13] >> 16) & 0xFFFF));
    a->drgbaDe[3] = (s32)((w[9] << 16) | (w[13] & 0xFFFF));
    a->drgbaDy[0] = (s32)((w[10] & 0xFFFF0000u) | ((w[14] >> 16) & 0xFFFF));
    a->drgbaDy[1] = (s32)((w[10] << 16) | (w[14] & 0xFFFF));
    a->drgbaDy[2] = (s32)((w[11] & 0xFFFF0000u) | ((w[15] >> 16) & 0xFFFF));
    a->drgbaDy[3] = (s32)((w[11] << 16) | (w[15] & 0xFFFF));
}

static void decode_tex(const u32 *w, AttrSetup *a)
{
    a->stzw[0] = (s32)((w[0] & 0xFFFF0000u) | ((w[4] >> 16) & 0xFFFF));
    a->stzw[1] = (s32)(((w[0] << 16) & 0xFFFF0000u) | (w[4] & 0xFFFF));
    a->stzw[3] = (s32)((w[1] & 0xFFFF0000u) | ((w[5] >> 16) & 0xFFFF));
    a->dstzwDx[0] = (s32)((w[2] & 0xFFFF0000u) | ((w[6] >> 16) & 0xFFFF));
    a->dstzwDx[1] = (s32)(((w[2] << 16) & 0xFFFF0000u) | (w[6] & 0xFFFF));
    a->dstzwDx[3] = (s32)((w[3] & 0xFFFF0000u) | ((w[7] >> 16) & 0xFFFF));
    a->dstzwDe[0] = (s32)((w[8] & 0xFFFF0000u) | ((w[12] >> 16) & 0xFFFF));
    a->dstzwDe[1] = (s32)(((w[8] << 16) & 0xFFFF0000u) | (w[12] & 0xFFFF));
    a->dstzwDe[3] = (s32)((w[9] & 0xFFFF0000u) | ((w[13] >> 16) & 0xFFFF));
    a->dstzwDy[0] = (s32)((w[10] & 0xFFFF0000u) | ((w[14] >> 16) & 0xFFFF));
    a->dstzwDy[1] = (s32)(((w[10] << 16) & 0xFFFF0000u) | (w[14] & 0xFFFF));
    a->dstzwDy[3] = (s32)((w[11] & 0xFFFF0000u) | ((w[15] >> 16) & 0xFFFF));
}

static void decode_z(const u32 *w, AttrSetup *a)
{
    a->stzw[2] = (s32)w[0];
    a->dstzwDx[2] = (s32)w[1];
    a->dstzwDe[2] = (s32)w[2];
    a->dstzwDy[2] = (s32)w[3];
}

static void rectangle(H64System *sys, H64RdpState *st, const u32 *w, int textured, int flipRect)
{
    TriSetup ts;
    AttrSetup a;
    u32 xl = (w[0] >> 12) & 0xFFF, yl = w[0] & 0xFFF, xh = (w[1] >> 12) & 0xFFF, yh = w[1] & 0xFFF;
    memset(&ts, 0, sizeof(ts));
    memset(&a, 0, sizeof(a));
    if (st->rasterFlags & (RS_COPY | RS_FILL)) yl |= 3;
    ts.xh = (s32)(xh << 13);
    ts.xl = (s32)(xl << 13);
    ts.xm = (s32)(xl << 13);
    ts.ym = (s32)yl;
    ts.yl = (s32)yl;
    ts.yh = (s32)yh;
    ts.flags = TS_FLIP;
    if (textured)
    {
        s32 s = (s32)((w[2] >> 16) & 0xFFFF), t = (s32)(w[2] & 0xFFFF);
        s32 dsdx = h64_sext((s32)((w[3] >> 16) & 0xFFFF), 16), dtdy = h64_sext((s32)(w[3] & 0xFFFF), 16);
        ts.tile = (w[1] >> 24) & 7;
        a.stzw[0] = s << 16;
        a.stzw[1] = t << 16;
        if (!flipRect)
        {
            a.dstzwDx[0] = dsdx << 11;
            a.dstzwDe[1] = dtdy << 11;
            a.dstzwDy[1] = dtdy << 11;
        }
        else
        {
            a.dstzwDx[1] = dtdy << 11;
            a.dstzwDe[0] = dsdx << 11;
            a.dstzwDy[0] = dsdx << 11;
        }
        if (st->rasterFlags & RS_COPY) ts.flags |= TS_SKIP_XFRAC;
    }
    draw(sys, st, &ts, &a);
}

static void set_other_modes(H64RdpState *st, const u32 *w)
{
    u32 f = st->rasterFlags & (RS_INTERLACE_FIELD | RS_INTERLACE_KEEP_ODD), db = 0;
    static const struct { int word, bit; u32 flag; } rs[] = {
        { 0, 19, RS_PERSPECTIVE }, { 0, 18, RS_DETAIL_LOD }, { 0, 17, RS_SHARPEN_LOD }, { 0, 16, RS_TEX_LOD },
        { 0, 15, RS_TLUT }, { 0, 14, RS_TLUT_TYPE }, { 0, 13, RS_SAMPLE_QUAD }, { 0, 12, RS_MID_TEXEL },
        { 0, 11, RS_BILERP0 }, { 0, 10, RS_BILERP1 }, { 0, 9, RS_CONVERT_ONE }, { 1, 13, RS_ALPHA_CVG_SELECT },
        { 1, 12, RS_CVG_TIMES_ALPHA }, { 1, 3, RS_AA }, { 1, 1, RS_ALPHA_TEST_DITHER }, { 1, 0, RS_ALPHA_TEST } };
    unsigned i;
    for (i = 0; i < sizeof(rs) / sizeof(rs[0]); i++)
        if (w[rs[i].word] & (1u << rs[i].bit)) f |= rs[i].flag;
    if (w[1] & (1 << 14)) db |= DB_FORCE_BLEND;
    if (w[1] & (1 << 7)) db |= DB_COLOR_ON_CVG;
    if (w[1] & (1 << 6)) db |= DB_IMAGE_READ;
    if (w[1] & (1 << 5)) db |= DB_DEPTH_UPDATE;
    if (w[1] & (1 << 4)) db |= DB_DEPTH_TEST;
    if (w[1] & (1 << 3)) db |= DB_AA;
    st->dither = (w[0] >> 4) & 0xF;
    if ((st->dither >> 2) != 3) db |= DB_DITHER;
    st->coverageMode = (u8)((w[1] >> 8) & 3);
    st->zMode = (u8)((w[1] >> 10) & 3);
    switch ((w[0] >> 20) & 3)
    {
    case 1: f |= RS_MULTI_CYCLE; db |= DB_MULTI_CYCLE; break;
    case 2: f |= RS_COPY; break;
    case 3: f |= RS_FILL; break;
    }
    st->blend[0][0] = (u8)((w[1] >> 30) & 3); st->blend[1][0] = (u8)((w[1] >> 28) & 3);
    st->blend[0][1] = (u8)((w[1] >> 26) & 3); st->blend[1][1] = (u8)((w[1] >> 24) & 3);
    st->blend[0][2] = (u8)((w[1] >> 22) & 3); st->blend[1][2] = (u8)((w[1] >> 20) & 3);
    st->blend[0][3] = (u8)((w[1] >> 18) & 3); st->blend[1][3] = (u8)((w[1] >> 16) & 3);
    st->rasterFlags = f;
    st->depthBlendFlags = db;
    st->usePrimDepth = (w[1] & (1 << 2)) != 0;
}

void h64_rdp_command(H64System *sys, const u64 *words, u32 count)
{
    H64RdpState *st = state(sys);
    u32 w[44], i, op;
    if (sys->options.noRdpDraw) return;
    for (i = 0; i < count && i < 22; i++)
    {
        w[i * 2] = (u32)(words[i] >> 32);
        w[i * 2 + 1] = (u32)words[i];
    }
    op = (w[0] >> 24) & 0x3F;
    switch (op)
    {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
    {
        TriSetup ts;
        AttrSetup a;
        const u32 *p = w + 8;
        memset(&a, 0, sizeof(a));
        decode_triangle(st, w, &ts);
        if (op & 4) { decode_rgba(p, &a); p += 16; }
        if (op & 2) { decode_tex(p, &a); p += 16; }
        if (op & 1) decode_z(p, &a);
        if (!sys->options.rdpStateOnly) draw(sys, st, &ts, &a);
        return;
    }
    case 0x24: if (!sys->options.rdpStateOnly) rectangle(sys, st, w, 1, 0); return;
    case 0x25: if (!sys->options.rdpStateOnly) rectangle(sys, st, w, 1, 1); return;
    case 0x36: if (!sys->options.rdpStateOnly) rectangle(sys, st, w, 0, 0); return;
    case 0x26: case 0x27: case 0x28: case 0x29: return;   // syncs (SYNC_FULL handled by the interface)
    case 0x2A:
        st->keyWidth[1] = (w[0] >> 12) & 0xFFF; st->keyWidth[2] = w[0] & 0xFFF;
        st->keyCenter[1] = (w[1] >> 24) & 0xFF; st->keyScale[1] = (w[1] >> 16) & 0xFF;
        st->keyCenter[2] = (w[1] >> 8) & 0xFF; st->keyScale[2] = w[1] & 0xFF;
        return;
    case 0x2B:
        st->keyWidth[0] = (w[1] >> 16) & 0xFFF; st->keyCenter[0] = (w[1] >> 8) & 0xFF; st->keyScale[0] = w[1] & 0xFF;
        return;
    case 0x2C:
    {
        u64 m = (u64)w[0] << 32 | w[1];
        st->convert[0] = 2 * h64_sext((s32)((m >> 45) & 0x1FF), 9) + 1;
        st->convert[1] = 2 * h64_sext((s32)((m >> 36) & 0x1FF), 9) + 1;
        st->convert[2] = 2 * h64_sext((s32)((m >> 27) & 0x1FF), 9) + 1;
        st->convert[3] = 2 * h64_sext((s32)((m >> 18) & 0x1FF), 9) + 1;
        st->convert[4] = (s32)((m >> 9) & 0x1FF);
        st->convert[5] = (s32)(m & 0x1FF);
        return;
    }
    case 0x2D:
        st->scissorXlo = (w[0] >> 12) & 0xFFF; st->scissorYlo = w[0] & 0xFFF;
        st->scissorXhi = (w[1] >> 12) & 0xFFF; st->scissorYhi = w[1] & 0xFFF;
        st->rasterFlags &= ~(RS_INTERLACE_FIELD | RS_INTERLACE_KEEP_ODD);
        if (w[1] & (1 << 25)) st->rasterFlags |= RS_INTERLACE_FIELD;
        if (w[1] & (1 << 24)) st->rasterFlags |= RS_INTERLACE_KEEP_ODD;
        return;
    case 0x2E:
        st->primDepth = (s32)(((w[1] >> 16) & 0x7FFF) << 16);
        st->primDz = w[1] & 0xFFFF;
        return;
    case 0x2F: set_other_modes(st, w); return;
    case 0x30: h64_rdp_load(sys, (w[1] >> 24) & 7, (w[0] >> 12) & 0xFFF, w[0] & 0xFFF, (w[1] >> 12) & 0xFFF, w[1] & 0xFFF, 1); return;
    case 0x32:
    {
        H64RdpTile *t = &st->tiles[(w[1] >> 24) & 7];
        t->slo = (w[0] >> 12) & 0xFFF; t->tlo = w[0] & 0xFFF; t->shi = (w[1] >> 12) & 0xFFF; t->thi = w[1] & 0xFFF;
        return;
    }
    case 0x33: h64_rdp_load(sys, (w[1] >> 24) & 7, (w[0] >> 12) & 0xFFF, w[0] & 0xFFF, (w[1] >> 12) & 0xFFF, w[1] & 0xFFF, 2); return;
    case 0x34: h64_rdp_load(sys, (w[1] >> 24) & 7, (w[0] >> 12) & 0xFFF, w[0] & 0xFFF, (w[1] >> 12) & 0xFFF, w[1] & 0xFFF, 0); return;
    case 0x35:
    {
        H64RdpTile *t = &st->tiles[(w[1] >> 24) & 7];
        t->offset = (w[0] & 511) << 3;
        t->stride = ((w[0] >> 9) & 511) << 3;
        t->size = (u8)((w[0] >> 19) & 3);
        t->fmt = (u8)((w[0] >> 21) & 7);
        t->palette = (u8)((w[1] >> 20) & 15);
        t->shiftS = (u8)(w[1] & 15);
        t->maskS = (u8)((w[1] >> 4) & 15);
        t->shiftT = (u8)((w[1] >> 10) & 15);
        t->maskT = (u8)((w[1] >> 14) & 15);
        t->flags = 0;
        if (w[1] & (1 << 8)) t->flags |= TILE_MIRROR_S;
        if (w[1] & (1 << 9)) t->flags |= TILE_CLAMP_S;
        if (w[1] & (1 << 18)) t->flags |= TILE_MIRROR_T;
        if (w[1] & (1 << 19)) t->flags |= TILE_CLAMP_T;
        if (t->maskS > 10) t->maskS = 10; else if (t->maskS == 0) t->flags |= TILE_CLAMP_S;
        if (t->maskT > 10) t->maskT = 10; else if (t->maskT == 0) t->flags |= TILE_CLAMP_T;
        return;
    }
    case 0x37: st->fillColor = w[1]; return;
    case 0x38: st->fogColor = w[1]; return;
    case 0x39: st->blendColor = w[1]; return;
    case 0x3A: st->primMinLevel = (u8)((w[0] >> 8) & 31); st->primLodFrac = (u8)w[0]; st->primColor = w[1]; return;
    case 0x3B: st->envColor = w[1]; return;
    case 0x3C:
    {
        H64RdpCombiner *c0 = &st->combiner[0], *c1 = &st->combiner[1];
        c0->rgbMulAdd = (u8)((w[0] >> 20) & 0xF); c0->rgbMul = (u8)((w[0] >> 15) & 0x1F);
        c0->rgbMulSub = (u8)((w[1] >> 28) & 0xF); c0->rgbAdd = (u8)((w[1] >> 15) & 0x7);
        c0->aMulAdd = (u8)((w[0] >> 12) & 0x7); c0->aMulSub = (u8)((w[1] >> 12) & 0x7);
        c0->aMul = (u8)((w[0] >> 9) & 0x7); c0->aAdd = (u8)((w[1] >> 9) & 0x7);
        c1->rgbMulAdd = (u8)((w[0] >> 5) & 0xF); c1->rgbMul = (u8)(w[0] & 0x1F);
        c1->rgbMulSub = (u8)((w[1] >> 24) & 0xF); c1->rgbAdd = (u8)((w[1] >> 6) & 0x7);
        c1->aMulAdd = (u8)((w[1] >> 21) & 0x7); c1->aMulSub = (u8)((w[1] >> 3) & 0x7);
        c1->aMul = (u8)((w[1] >> 18) & 0x7); c1->aAdd = (u8)(w[1] & 0x7);
        return;
    }
    case 0x3D:
        st->texFmt = (u8)((w[0] >> 21) & 7);
        st->texSize = (u8)((w[0] >> 19) & 3);
        st->texWidth = (w[0] & 0x3FF) + 1;
        st->texAddr = w[1] & 0xFFFFFF;
        return;
    case 0x3E: st->depthAddr = w[1] & 0xFFFFFF; return;
    case 0x3F:
    {
        u32 fmt = (w[0] >> 21) & 7, size = (w[0] >> 19) & 3;
        st->colorWidth = (w[0] & 1023) + 1;
        st->colorAddr = w[1] & 0xFFFFFF;
        st->colorFmt = size == 0 ? FB_I4 : size == 1 ? FB_I8 : size == 2 ? (fmt ? FB_IA88 : FB_RGBA5551) : FB_RGBA8888;
        return;
    }
    }
}

void h64_rdp_free(H64System *sys)
{
    free(sys->rdpState);
    free(sys->rdramHidden);
    sys->rdpState = 0;
    sys->rdramHidden = 0;
}
