// Harissa64 V2 - VI output stage.
//
// Serial CPU port of ParaLLEl-RDP's VI (MIT licence, Themaister/parallel-rdp
// commit 1cecd042, see THIRD_PARTY.md): register decoding
// (video_interface.cpp decode_vi_registers, scanout), the RDRAM fetch with
// coverage (shaders/extract_vram.comp), the anti-alias and dither filters
// (vi_fetch.frag), the divot filter (vi_divot.frag), and the scaler with
// gamma and gamma dither (vi_scale.frag). Pixels are computed on demand.
//
// Simplifications for now: the VI registers are taken as constant for the
// whole frame (no per-scanline changes); an interlaced (serrate) frame is
// produced with both fields from the current registers; the "fetch bug"
// (Y-interpolation quirk when YAdd < 1.0) is not emulated.
#include "h64_vi.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../rdp/h64_rdp_luts.h"
#include "../system/h64_system.h"

enum { VI_CTRL_TYPE_MASK = 3, VI_CTRL_GAMMA_DITHER = 1 << 2, VI_CTRL_GAMMA = 1 << 3, VI_CTRL_DIVOT = 1 << 4,
       VI_CTRL_SERRATE = 1 << 6, VI_CTRL_AA_MASK = 3 << 8, VI_CTRL_DITHER_FILTER = 1 << 16 };

struct ViFrame
{
    H64System *sys;
    u32 status, width, origin;
    int is32, fetchAa, scaleAa, divot, ditherFilter, gamma, gammaDither;
};

struct Col { u32 r, g, b, a; };

static s32 h64_clamp_s32(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

static Col fetch(const ViFrame *f, s32 x, s32 y)
{
    Col c;
    s32 lin = y * (s32)f->width + x;
    if (f->is32)
    {
        u32 idx = ((u32)(lin + (s32)(f->origin >> 2))) & (H64_RDRAM_SIZE / 4 - 1);
        u32 w = h64_load_be32(f->sys->rdram + idx * 4);
        c.r = w >> 24; c.g = (w >> 16) & 0xFF; c.b = (w >> 8) & 0xFF; c.a = (w >> 5) & 7;
    }
    else
    {
        u32 idx = ((u32)(lin + (s32)(f->origin >> 1))) & (H64_RDRAM_SIZE / 2 - 1);
        u32 w = h64_load_be16(f->sys->rdram + idx * 2);
        u32 hidden = f->sys->rdramHidden ? f->sys->rdramHidden[idx] : 3;
        c.r = (w >> 8) & 0xF8; c.g = (w >> 3) & 0xF8; c.b = (w << 2) & 0xF8;
        c.a = ((w & 1) << 2) | hidden;
    }
    if (!f->fetchAa) c.a = 7;
    return c;
}

static void check_neighbor(const Col *cand, u32 *lo, u32 *hi, u32 *slo, u32 *shi)
{
    int i;
    if (cand->a != 7) return;
    for (i = 0; i < 3; i++)
    {
        u32 v = i == 0 ? cand->r : i == 1 ? cand->g : cand->b;
        u32 m = v > lo[i] ? v : lo[i], n = v < hi[i] ? v : hi[i];
        if (m < slo[i]) slo[i] = m;
        if (n > shi[i]) shi[i] = n;
        if (v < lo[i]) lo[i] = v;
        if (v > hi[i]) hi[i] = v;
    }
}

// vi_fetch.frag: anti-alias filter on partially covered pixels, dither filter otherwise.
static Col aa(const ViFrame *f, s32 x, s32 y)
{
    Col mid = fetch(f, x, y), out = mid;
    u32 m[3];
    int i;
    m[0] = mid.r; m[1] = mid.g; m[2] = mid.b;
    if (mid.a != 7)
    {
        u32 lo[3], hi[3], slo[3], shi[3];
        static const s32 nb[6][2] = { { -1, -1 }, { 1, -1 }, { -2, 0 }, { 2, 0 }, { -1, 1 }, { 1, 1 } };
        for (i = 0; i < 3; i++) lo[i] = hi[i] = slo[i] = shi[i] = m[i];
        for (i = 0; i < 6; i++)
        {
            Col n = fetch(f, x + nb[i][0], y + nb[i][1]);
            check_neighbor(&n, lo, hi, slo, shi);
        }
        for (i = 0; i < 3; i++)
        {
            u32 off = slo[i] + shi[i] - (m[i] << 1);
            m[i] = (m[i] + ((off * (7 - mid.a) + 4) >> 3)) & 0xFF;
        }
    }
    else if (f->ditherFilter)
    {
        s32 t[3], acc[3] = { 0, 0, 0 }, dx, dy;
        static const s32 extra[3][2] = { { -1, 1 }, { 1, 1 }, { 0, 1 } };
        for (i = 0; i < 3; i++) t[i] = (s32)(m[i] >> 3);
        for (dy = -1; dy <= 0; dy++)
            for (dx = -1; dx <= 1; dx++)
            {
                Col n = fetch(f, x + dx, y + dy);
                acc[0] += h64_clamp_s32((s32)(n.r >> 3) - t[0], -1, 1);
                acc[1] += h64_clamp_s32((s32)(n.g >> 3) - t[1], -1, 1);
                acc[2] += h64_clamp_s32((s32)(n.b >> 3) - t[2], -1, 1);
            }
        for (i = 0; i < 3; i++)
        {
            Col n = fetch(f, x + extra[i][0], y + extra[i][1]);
            acc[0] += h64_clamp_s32((s32)(n.r >> 3) - t[0], -1, 1);
            acc[1] += h64_clamp_s32((s32)(n.g >> 3) - t[1], -1, 1);
            acc[2] += h64_clamp_s32((s32)(n.b >> 3) - t[2], -1, 1);
        }
        for (i = 0; i < 3; i++) m[i] = (u32)((s32)(m[i] & 0xF8) + acc[i]);
    }
    out.r = m[0]; out.g = m[1]; out.b = m[2];
    return out;
}

static u32 median3(u32 l, u32 c, u32 r)
{
    u32 t;
    if (l < c) { t = l; l = c; c = t; }
    if (c < r) { t = c; c = r; r = t; }
    if (l < c) { t = l; l = c; c = t; }
    return c;
}

// vi_divot.frag
static Col divot(const ViFrame *f, s32 x, s32 y)
{
    Col l, m, r, o;
    m = aa(f, x, y);
    if (!f->divot) return m;
    l = aa(f, x - 1, y);
    r = aa(f, x + 1, y);
    if ((l.a & m.a & r.a) == 7) return m;
    o.r = median3(l.r, m.r, r.r);
    o.g = median3(l.g, m.g, r.g);
    o.b = median3(l.b, m.b, r.b);
    o.a = m.a;
    return o;
}

static u32 vi_lerp(u32 a, u32 b, u32 l) { return (a + (((b - a) * l + 16) >> 5)) & 0xFF; }

static u16 noise_seed(u32 x, u32 y, u32 frame)
{
    const u32 P = 1103515245u;
    u32 s0 = x, s1 = y, s2 = frame, n0, n1, n2, r;
    for (r = 0; r < 3; r++)
    {
        n0 = ((s0 >> 8) ^ s1) * P; n1 = ((s1 >> 8) ^ s2) * P; n2 = ((s2 >> 8) ^ s0) * P;
        s0 = n0; s1 = n1; s2 = n2;
    }
    return (u16)(s0 >> 16);
}

int h64_vi_render(H64System *sys, u8 *rgb, int maxW, int maxH, int *outW, int *outH)
{
    const u32 *v = sys->vi.regs;
    ViFrame f;
    s32 vStart, vEnd, vSync, yStart, yAdd, vRes, vOffset, hOffset, xStart, xAdd, hStart, hEnd;
    s32 hStartClamp, hEndClamp, width, height, serrate, x, y, isPal, leftClamp = 0, rightClamp = 0;
    u32 aaMode;
    memset(&f, 0, sizeof(f));
    f.sys = sys;
    f.status = v[0];
    f.width = v[2] & 0xFFF;
    f.origin = v[1] & 0xFFFFFF;
    if ((f.status & VI_CTRL_TYPE_MASK) < 2 || f.origin == 0) return -1;   // blank
    f.is32 = (f.status & VI_CTRL_TYPE_MASK) == 3;
    aaMode = f.status & VI_CTRL_AA_MASK;
    f.fetchAa = aaMode < (2u << 8);
    f.scaleAa = aaMode < (3u << 8);
    f.divot = (f.status & VI_CTRL_DIVOT) != 0;
    f.ditherFilter = (f.status & VI_CTRL_DITHER_FILTER) != 0;
    f.gamma = (f.status & VI_CTRL_GAMMA) != 0;
    f.gammaDither = (f.status & VI_CTRL_GAMMA_DITHER) != 0;
    f.origin &= ~(u32)(f.is32 ? 3 : 1);

    // decode_vi_registers
    vStart = (s32)((v[10] >> 16) & 0x3FF);
    vEnd = (s32)(v[10] & 0x3FF);
    vSync = (s32)(v[6] & 0x3FF);
    yStart = (s32)((v[13] >> 16) & 0xFFF);
    yAdd = (s32)(v[13] & 0xFFF);
    isPal = vSync > 525 + 25;
    {
        s32 vEndMax = isPal ? ((44 + 576) | 1) : ((34 + 480) | 1);
        if (vEnd > vEndMax) vEnd = vEndMax;
        if (vStart > vEndMax) vStart = vEndMax;
    }
    vOffset = isPal ? 44 : 34;
    hOffset = isPal ? 128 : 108;
    vRes = (vEnd - vStart) >> 1;
    vStart = (vStart - vOffset) / 2;
    if (vStart < 0) { yStart -= yAdd * vStart; vStart = 0; }
    if (vRes > 288 - vStart) vRes = 288 - vStart;
    xStart = (s32)((v[12] >> 16) & 0xFFF);
    xAdd = (s32)(v[12] & 0xFFF);
    hStart = (s32)((v[9] >> 16) & 0x3FF) - hOffset;
    hEnd = (s32)(v[9] & 0x3FF) - hOffset;
    if (hStart < 0) { xStart -= xAdd * hStart; hStart = 0; leftClamp = 1; }
    if (hEnd > 640) { hEnd = 640; rightClamp = 1; }
    hStartClamp = hStart + (leftClamp ? 0 : 8);
    hEndClamp = hEnd - (rightClamp ? 0 : 7);
    if (hEnd - hStart <= 0 || hStart >= 640 || vRes <= 0) return -1;

    serrate = (f.status & VI_CTRL_SERRATE) != 0;
    width = 640;
    height = (isPal ? 576 : 480) >> (serrate ? 0 : 1);
    if (width > maxW || height > maxH) return -1;
    memset(rgb, 0, (size_t)width * height * 3);

    for (y = 0; y < height; y++)
    {
        s32 line = serrate ? y >> 1 : y, cy;
        if (line < vStart || line >= vStart + vRes) continue;
        cy = line - vStart;
        for (x = hStartClamp; x < hEndClamp && x < width; x++)
        {
            s32 cx = x - hStart, sx = cx * xAdd + xStart, sy = cy * yAdd + yStart, bx = sx >> 10, by = sy >> 10;
            Col c00 = divot(&f, bx, by);
            u32 c[3];
            u16 noise = 0;
            u8 *o = rgb + ((size_t)y * width + x) * 3;
            c[0] = c00.r; c[1] = c00.g; c[2] = c00.b;
            if (f.gammaDither) noise = noise_seed((u32)cx, (u32)cy, sys->vi.frames);
            if (f.scaleAa)
            {
                u32 xf = (u32)(sx >> 5) & 31, yf = (u32)(sy >> 5) & 31;
                Col c10 = divot(&f, bx + 1, by), c01 = divot(&f, bx, by + 1), c11 = divot(&f, bx + 1, by + 1);
                u32 a0[3], a1[3];
                a0[0] = vi_lerp(c00.r, c01.r, yf); a0[1] = vi_lerp(c00.g, c01.g, yf); a0[2] = vi_lerp(c00.b, c01.b, yf);
                a1[0] = vi_lerp(c10.r, c11.r, yf); a1[1] = vi_lerp(c10.g, c11.g, yf); a1[2] = vi_lerp(c10.b, c11.b, yf);
                c[0] = vi_lerp(a0[0], a1[0], xf); c[1] = vi_lerp(a0[1], a1[1], xf); c[2] = vi_lerp(a0[2], a1[2], xf);
            }
            if (f.gamma)
            {
                if (f.gammaDither)
                {
                    u32 d[3];
                    d[0] = noise & 0x3F; d[1] = (noise >> 6) & 0x3F; d[2] = ((noise >> 9) & 0x38) | (noise & 7);
                    c[0] = h64_vi_gamma_table[(c[0] << 6) + d[0] + 256];
                    c[1] = h64_vi_gamma_table[(c[1] << 6) + d[1] + 256];
                    c[2] = h64_vi_gamma_table[(c[2] << 6) + d[2] + 256];
                }
                else
                {
                    c[0] = h64_vi_gamma_table[c[0] & 0xFF];
                    c[1] = h64_vi_gamma_table[c[1] & 0xFF];
                    c[2] = h64_vi_gamma_table[c[2] & 0xFF];
                }
            }
            else if (f.gammaDither)
            {
                int i;
                for (i = 0; i < 3; i++)
                {
                    c[i] += (noise >> i) & 1;
                    if (c[i] > 0xFF) c[i] = 0xFF;
                }
            }
            o[0] = (u8)c[0]; o[1] = (u8)c[1]; o[2] = (u8)c[2];
        }
    }
    *outW = width;
    *outH = height;
    return 0;
}

// Raw framebuffer capture (no VI processing), kept for debugging.
int h64_vi_capture(H64System *sys, u8 *rgb, int maxW, int maxH, int *w, int *h)
{
    u32 status = sys->vi.regs[0], origin = sys->vi.regs[1] & 0xFFFFFF, width = sys->vi.regs[2] & 0xFFF;
    u32 vVideo = sys->vi.regs[10], yScale = sys->vi.regs[13] & 0xFFF;
    int type = (int)(status & 3), bpp, lines, x, y;
    u32 vStart = (vVideo >> 16) & 0x3FF, vEnd = vVideo & 0x3FF;

    if (type < 2 || width == 0)
        return -1;
    bpp = type == 3 ? 4 : 2;
    lines = vEnd > vStart ? (int)((vEnd - vStart) / 2) : 240;
    if (yScale) lines = (int)((u32)lines * yScale / 1024);
    if (lines <= 0) lines = 240;
    if ((int)width > maxW) width = (u32)maxW;
    if (lines > maxH) lines = maxH;

    for (y = 0; y < lines; y++)
        for (x = 0; x < (int)width; x++)
        {
            u32 addr = origin + ((u32)y * (sys->vi.regs[2] & 0xFFF) + (u32)x) * (u32)bpp;
            u8 *out = rgb + ((size_t)y * width + (size_t)x) * 3;
            if (addr + (u32)bpp > H64_RDRAM_SIZE) { out[0] = out[1] = out[2] = 0; continue; }
            if (bpp == 2)
            {
                u16 p = h64_load_be16(sys->rdram + addr);   // RGBA 5551
                out[0] = (u8)(((p >> 11) & 31) << 3);
                out[1] = (u8)(((p >> 6) & 31) << 3);
                out[2] = (u8)(((p >> 1) & 31) << 3);
            }
            else
            {
                out[0] = sys->rdram[addr];
                out[1] = sys->rdram[addr + 1];
                out[2] = sys->rdram[addr + 2];
            }
        }
    *w = (int)width;
    *h = lines;
    return 0;
}
