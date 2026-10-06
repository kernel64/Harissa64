// Harissa64 V2 - software RDP: TMEM loads, texture sampling and LOD.
//
// Sampling and LOD are ported from ParaLLEl-RDP's shaders/texture.h (MIT,
// see h64_rdp_state.h). The shader addresses TMEM "physically" (bytes XOR 3,
// halfwords XOR 1, as 32-bit little-endian words); tm8()/tm16() map those
// indices onto our TMEM, which is kept in N64 byte order, so the expressions
// below stay as in the original.
//
// Loads (LOAD_TILE, LOAD_BLOCK, LOAD_TLUT) are written from the hardware
// description (n64brew "Reality Display Processor/Commands"): the RDP walks
// the source in 64-bit words, writes them to TMEM, and swaps the 32-bit halves
// of every word on odd lines; 32-bit and YUV textures are split between the
// lower (RG / UV) and upper (BA / Y) halves of TMEM; TLUT entries are
// written four times (one per bank) in the upper half.
#include "h64_rdp_state.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"

enum { TEX_RGBA = 0, TEX_YUV = 1, TEX_CI = 2, TEX_IA = 3, TEX_I = 4 };

// ---- TMEM access (shader "physical" indices) ----
static u32 tm8(const H64RdpState *st, u32 p) { return st->tmem[(p ^ 3) & 0xFFF]; }
static u32 tm16(const H64RdpState *st, u32 p)
{
    u32 a = ((p ^ 1) & 0x7FF) * 2;
    return (u32)st->tmem[a] << 8 | st->tmem[a + 1];
}
static void tm16_write_logical(H64RdpState *st, u32 h, u32 v)
{
    u32 a = (h & 0x7FF) * 2;
    st->tmem[a] = (u8)(v >> 8);
    st->tmem[a + 1] = (u8)v;
}

// ---- Loads ----
static u8 rd8(H64System *sys, u32 a) { return sys->rdram[a & (H64_RDRAM_SIZE - 1)]; }

void h64_rdp_load(H64System *sys, u32 tileIndex, u32 sl, u32 tl, u32 sh, u32 th, int mode)
{
    H64RdpState *st = sys->rdpState;
    H64RdpTile *tile = &st->tiles[tileIndex & 7];
    u32 texSize = st->texSize;
    tile->slo = sl; tile->shi = sh; tile->tlo = tl; tile->thi = th;
    if (texSize == 0)
    {
        H64_DEBUG("[rdp] load from a 4-bit texture image (crashes the RDP), ignored");
        return;
    }

    if (mode == 1)   // LOAD_TLUT: one line of 16-bit entries, each written to the four banks
    {
        u32 count = ((sh >> 2) - (sl >> 2) + 1) & 0xFFF, i, k;
        u32 src = st->texAddr + (((tl >> 2) * st->texWidth + (sl >> 2)) << (texSize - 1));
        u32 h = (tile->offset >> 1) & 0x7FF;
        if (count > 256) count = 256;
        for (i = 0; i < count; i++)
        {
            u32 v = (u32)rd8(sys, src + i * 2) << 8 | rd8(sys, src + i * 2 + 1);
            for (k = 0; k < 4; k++) tm16_write_logical(st, h + i * 4 + k, v);
        }
        return;
    }

    {
        int split = tile->size == 3 || tile->fmt == TEX_YUV;   // lower/upper halves
        u32 lines, wordsPerLine, srcStride, y, w;
        u32 srcBase, tmemBase = tile->offset;
        u32 dxtCounter = 0;
        if (mode == 2)   // LOAD_BLOCK: sh is the last texel, th is dxt (1.11)
        {
            u32 texels = (sh - sl + 1) & 0xFFF;
            if (!texels || texels > 2048) return;
            lines = 1;
            wordsPerLine = ((texels << texSize) + 15) >> 4;   // 64-bit words, in the image's texel size
            srcBase = st->texAddr + ((tl * st->texWidth + sl) << (texSize - 1));
            srcStride = 0;
        }
        else
        {
            u32 pixels = ((sh >> 2) - (sl >> 2) + 1) & 0xFFF;
            if ((th >> 2) < (tl >> 2) || !pixels) return;
            lines = (th >> 2) - (tl >> 2) + 1;
            wordsPerLine = ((pixels << texSize) + 15) >> 4;
            srcBase = st->texAddr + (((tl >> 2) * st->texWidth + (sl >> 2)) << (texSize - 1));
            srcStride = st->texWidth << (texSize - 1);
        }

        for (y = 0; y < lines; y++)
        {
            u32 src = srcBase + y * srcStride;
            for (w = 0; w < wordsPerLine; w++)
            {
                u32 t, odd, b;
                u8 word[8];
                for (b = 0; b < 8; b++) word[b] = rd8(sys, src + w * 8 + b);
                if (mode == 2)
                {
                    t = dxtCounter >> 11;
                    dxtCounter += th;
                }
                else
                    t = y;
                odd = t & 1;
                if (!split)
                {
                    // Word w of line t goes to offset + t * stride + w * 8; odd lines swap halves.
                    u32 dst = (tmemBase + t * tile->stride + w * 8) & 0xFFF;
                    for (b = 0; b < 8; b++) st->tmem[(dst + (b ^ (odd << 2))) & 0xFFF] = word[b];
                }
                else
                {
                    // Two 32-bit source units per word: lower half gets RG (or UV), upper half BA (or YY).
                    u32 u;
                    for (u = 0; u < 2; u++)
                    {
                        const u8 *s = word + u * 4;
                        u32 lo, hi, idx;
                        if (tile->fmt == TEX_YUV) { lo = (u32)s[0] << 8 | s[2]; hi = (u32)s[1] << 8 | s[3]; }
                        else { lo = (u32)s[0] << 8 | s[1]; hi = (u32)s[2] << 8 | s[3]; }
                        // Halfword index in each half: the 16-bit layout of 2 texels per 32 bits.
                        idx = ((tmemBase >> 1) + t * (tile->stride >> 1) + w * 2 + u) & 0x3FF;
                        idx ^= odd << 1;
                        tm16_write_logical(st, idx, lo);
                        tm16_write_logical(st, idx | 0x400, hi);
                    }
                }
            }
        }
    }
}

// ---- Texel fetch (texture.h) ----
static s32 mask_coord(int mask, int mirror, s32 v)
{
    if (mask)
    {
        s32 m = 1 << mask;
        if (mirror) { s32 x = (v & m) - 1; v ^= x > 0 ? x : 0; }
        v &= m - 1;
    }
    return v;
}
static s32 mask_s(const H64RdpTile *t, s32 s) { return mask_coord(t->maskS, (t->flags & TILE_MIRROR_S) != 0, s); }
static s32 mask_t(const H64RdpTile *t, s32 v) { return mask_coord(t->maskT, (t->flags & TILE_MIRROR_T) != 0, v); }

static void convert_rgba16(u32 w, H64RdpTexel *o)
{
    u32 r = (w >> 11) & 31, g = (w >> 6) & 31, b = (w >> 1) & 31;
    o->c[0] = (s32)((r << 3) | (r >> 2));
    o->c[1] = (s32)((g << 3) | (g >> 2));
    o->c[2] = (s32)((b << 3) | (b >> 2));
    o->c[3] = (s32)((w & 1) * 0xFF);
}
static void convert_ia16(u32 w, H64RdpTexel *o)
{
    o->c[0] = o->c[1] = o->c[2] = (s32)(w >> 8);
    o->c[3] = (s32)(w & 0xFF);
}
static void splat(H64RdpTexel *o, s32 v) { o->c[0] = o->c[1] = o->c[2] = o->c[3] = v; }

static u32 index8(const H64RdpTile *t, u32 s, u32 tt, u32 xoff, u32 mask)
{
    u32 bo = (t->offset + t->stride * tt + xoff) & mask;
    return bo ^ ((tt & 1) << 2) ^ 3;
}
static u32 index16(const H64RdpTile *t, u32 tt, u32 byteX, u32 mask)
{
    u32 bo = (t->offset + t->stride * tt + byteX) & mask;
    return (bo >> 1) ^ ((tt & 1) << 1) ^ 1;
}

// Fetch one texel at (s, t). Returns 0 when the format combination is not sampled here.
static void fetch(const H64RdpState *st, const H64RdpTile *tile, u32 s, u32 t, int tlut, int tlutType, u32 lutOffset,
                  u32 addrXor, H64RdpTexel *o)
{
    u32 w;
    if (tlut)
    {
        u32 lut;
        if (tile->size == 0)
        {
            w = tm8(st, index8(tile, s, t, s >> 1, 0x7FF));
            w = (w >> ((~s & 1) * 4)) & 0xF;
            w |= (u32)tile->palette << 4;
            lut = (w << 2) + lutOffset;
        }
        else if (tile->size == 1)
        {
            w = tm8(st, index8(tile, s, t, s, 0x7FF));
            lut = (w << 2) + lutOffset;
        }
        else
        {
            w = tm16(st, index16(tile, t, s * 2, 0x7FF));
            lut = ((w >> 6) & ~3u) + lutOffset;
        }
        lut ^= addrXor;
        w = tm16(st, 0x400 | lut);
        if (tlutType) convert_ia16(w, o);
        else convert_rgba16(w, o);
        return;
    }
    switch (tile->fmt)
    {
    case TEX_RGBA:
    case TEX_I:
    case TEX_CI:
    case TEX_IA:
    default:
        break;
    }
    if (tile->size == 0)
    {
        w = tm8(st, index8(tile, s, t, s >> 1, 0xFFF));
        w = (w >> ((~s & 1) * 4)) & 0xF;
        if (tile->fmt == TEX_CI) { splat(o, (s32)(w | (u32)tile->palette << 4)); return; }
        if (tile->fmt == TEX_IA)
        {
            u32 i = w & 0xE;
            i = (i << 4) | (i << 1) | (i >> 2);
            o->c[0] = o->c[1] = o->c[2] = (s32)i;
            o->c[3] = (s32)((w & 1) * 0xFF);
            return;
        }
        splat(o, (s32)(w | w << 4));   // RGBA4 / I4 (and the unusual YUV4)
        return;
    }
    if (tile->size == 1)
    {
        w = tm8(st, index8(tile, s, t, s, 0xFFF));
        if (tile->fmt == TEX_IA)
        {
            u32 i = w >> 4, a = w & 0xF;
            i |= i << 4;
            a |= a << 4;
            o->c[0] = o->c[1] = o->c[2] = (s32)i;
            o->c[3] = (s32)a;
            return;
        }
        splat(o, (s32)w);   // RGBA8 / I8 / CI8
        return;
    }
    if (tile->size == 2 && (tile->fmt == TEX_RGBA || tile->fmt == TEX_IA))
    {
        w = tm16(st, index16(tile, t, s * 2, 0xFFF));
        if (tile->fmt == TEX_RGBA) convert_rgba16(w, o);
        else convert_ia16(w, o);
        return;
    }
    if (tile->size == 3 && tile->fmt == TEX_RGBA)
    {
        u32 idx = index16(tile, t, s * 2, 0x7FF), lo = tm16(st, idx), hi = tm16(st, idx | 0x400);
        o->c[0] = (s32)(lo >> 8); o->c[1] = (s32)(lo & 0xFF); o->c[2] = (s32)(hi >> 8); o->c[3] = (s32)(hi & 0xFF);
        return;
    }
    // "ci32": CI16/I16/IA32/CI32/I32 read a 16-bit word as two bytes (xyxy).
    w = tm16(st, index16(tile, t, s * 2, 0xFFF));
    o->c[0] = o->c[2] = (s32)(w >> 8);
    o->c[1] = o->c[3] = (s32)(w & 0xFF);
}

static void fetch_yuv(const H64RdpState *st, const H64RdpTile *tile, u32 s, u32 t, u32 chromaX, H64RdpTexel *o)
{
    u32 bo = tile->offset + tile->stride * t;
    u32 il = (((bo + s) & 0x7FF) ^ ((t & 1) << 2) ^ 3);
    u32 ic = ((((bo + chromaX * 2) & 0x7FF) >> 1) ^ ((t & 1) << 1) ^ 1);
    u32 luma = tm8(st, il | 0x800), chroma = tm16(st, ic);
    o->c[0] = (s32)((chroma >> 8) & 0xFF) - 0x80;
    o->c[1] = (s32)(chroma & 0xFF) - 0x80;
    o->c[2] = o->c[3] = (s32)luma;
}

static s32 clamp_and_shift(int clampBit, s32 coord, s32 lo, s32 hi, int shift)
{
    coord = h64_clamp(coord, -0x8000, 0x7FFF);
    if (shift < 11) coord >>= shift;
    else { coord = (s32)((u32)coord << (32 - shift)); coord >>= 16; }
    if (clampBit)
    {
        if ((coord >> 3) >= hi) coord = (((hi >> 2) - (lo >> 2)) & 0x3FF) << 5;
        else { coord -= lo << 3; if (coord < 0) coord = 0; }
    }
    else
        coord -= lo << 3;
    return coord;
}

static s32 shift_only(s32 coord, s32 lo, int shift)
{
    coord = h64_clamp(coord, -0x8000, 0x7FFF);
    if (shift < 11) coord >>= shift;
    else { coord = (s32)((u32)coord << (32 - shift)); coord >>= 16; }
    return coord - (lo << 3);
}

void h64_rdp_texture_convert(const H64RdpTexel *in, const s32 *f, H64RdpTexel *out)
{
    s32 r = h64_sext(in->c[0], 9), g = h64_sext(in->c[1], 9), b = h64_sext(in->c[2], 9);
    H64RdpTexel o;
    o.c[0] = (s16)(b + ((f[0] * g + 0x80) >> 8));
    o.c[1] = (s16)(b + ((f[1] * r + f[2] * g + 0x80) >> 8));
    o.c[2] = (s16)(b + ((f[3] * r + 0x80) >> 8));
    o.c[3] = (s16)b;
    *out = o;
}

void h64_rdp_sample(H64RdpState *st, const H64RdpTile *tile, const s32 *stIn, int tlut, int tlutType, int sampleQuad,
                    int midTexelState, int convertOne, int bilerp, const s32 *factors, const H64RdpTexel *prev,
                    H64RdpTexel *out)
{
    s32 s = clamp_and_shift((tile->flags & TILE_CLAMP_S) != 0, stIn[0], (s32)tile->slo, (s32)tile->shi, tile->shiftS);
    s32 t = clamp_and_shift((tile->flags & TILE_CLAMP_T) != 0, stIn[1], (s32)tile->tlo, (s32)tile->thi, tile->shiftT);
    s32 fx = 0, fy = 0, sumFrac, s0, t0, s1, t1, tdiff, chromaFrac, i;
    int midTexel, upperLut, yuv = tile->fmt == TEX_YUV;
    H64RdpTexel tb, t10, t01, t11, acc;
    u32 bs, bt;
    memset(&t10, 0, sizeof(t10)); memset(&t01, 0, sizeof(t01)); memset(&t11, 0, sizeof(t11));
    if (sampleQuad || tlut) { fx = s & 31; fy = t & 31; }
    sumFrac = fx + fy;
    s >>= 5;
    t >>= 5;
    s0 = mask_s(tile, s);
    t0 = mask_t(tile, t);
    s1 = mask_s(tile, s + 1);
    t1 = mask_t(tile, t + 1);
    tdiff = t1 - t0;
    if (tdiff < -255) tdiff = -255;
    t1 = (t0 & 0xFF) + tdiff;
    t0 &= 0xFF;
    midTexel = midTexelState && bilerp && fx == 0x10 && fy == 0x10;
    upperLut = sumFrac >= 0x20;
    if (midTexel) sumFrac = 0;
    bs = (u32)(sumFrac >= 0x20 ? s1 : s0);
    bt = (u32)(sumFrac >= 0x20 ? t1 : t0);
    chromaFrac = ((s0 & 1) << 4) | (fx >> 1);

    if (tlut)
    {
        u32 addrXor = upperLut ? 2 : 1;
        int upper = sumFrac >= 0x20;
        if (!sampleQuad) { bs = (u32)s0; bt = (u32)t0; s1 = s0; t1 = t0; }
        fetch(st, tile, bs, bt, 1, tlutType, upper ? 3 : 0, addrXor, &tb);
        if (bilerp)
        {
            fetch(st, tile, (u32)s1, (u32)t0, 1, tlutType, 1, addrXor, &t10);
            fetch(st, tile, (u32)s0, (u32)t1, 1, tlutType, 2, addrXor, &t01);
        }
        if (midTexel) fetch(st, tile, (u32)s1, (u32)t1, 1, tlutType, 3, addrXor, &t11);
    }
    else if (yuv)
    {
        u32 cx0 = (u32)s0 >> 1, cx1 = (u32)(s1 + (s1 - s0)) >> 1;
        fetch_yuv(st, tile, (u32)s0, (u32)t0, cx0, &tb);
        if (sampleQuad)
        {
            fetch_yuv(st, tile, (u32)s1, (u32)t0, cx1, &t10);
            fetch_yuv(st, tile, (u32)s0, (u32)t1, cx0, &t01);
            fetch_yuv(st, tile, (u32)s1, (u32)t1, cx1, &t11);
        }
    }
    else
    {
        fetch(st, tile, bs, bt, 0, 0, 0, 0, &tb);
        if (sampleQuad)
        {
            fetch(st, tile, (u32)s1, (u32)t0, 0, 0, 0, 0, &t10);
            fetch(st, tile, (u32)s0, (u32)t1, 0, 0, 0, 0, &t01);
        }
        if (midTexel) fetch(st, tile, (u32)s1, (u32)t1, 0, 0, 0, 0, &t11);
    }

    if (convertOne)
    {
        s32 p[4];
        for (i = 0; i < 4; i++) p[i] = h64_sext(prev->c[i], 9);
        if (sampleQuad)
        {
            int midRg = yuv ? (midTexelState && chromaFrac == 0x10 && fy == 0x10) : midTexel;
            int midBa = midTexel, upperBa = sumFrac >= 32;
            int upperRg = yuv ? ((chromaFrac + fy) >= 32 && !midRg) : upperBa;
            s32 frg0 = upperRg ? p[1] : p[0], frg1 = upperRg ? p[0] : p[1];
            s32 fba0 = upperBa ? p[1] : p[0], fba1 = upperBa ? p[0] : p[1];
            s32 conv[4];
            for (i = 0; i < 2; i++)
            {
                if (midRg)
                    conv[i] = frg0 * (t01.c[i] - t11.c[i]) + frg1 * (t10.c[i] - t11.c[i]) + ((tb.c[i] - t11.c[i]) << 6) + 0x80;
                else
                {
                    s32 base = upperRg && yuv ? t11.c[i] : tb.c[i];
                    conv[i] = frg0 * (t10.c[i] - base) + frg1 * (t01.c[i] - base) + 0x80;
                }
            }
            for (i = 2; i < 4; i++)
            {
                if (midBa)
                    conv[i] = fba0 * (t01.c[i] - t11.c[i]) + fba1 * (t10.c[i] - t11.c[i]) + ((tb.c[i] - t11.c[i]) << 6) + 0x80;
                else
                {
                    s32 base = upperBa && yuv ? t11.c[i] : tb.c[i];
                    conv[i] = fba0 * (t10.c[i] - base) + fba1 * (t01.c[i] - base) + 0x80;
                }
            }
            for (i = 0; i < 4; i++) acc.c[i] = (s16)((conv[i] >> 8) + p[2]);
        }
        else
            splat(&acc, (s16)p[2]);
    }
    else if (yuv)
    {
        if (sampleQuad)
        {
            if (bilerp)
            {
                int midChroma = midTexelState && chromaFrac == 0x10 && fy == 0x10;
                for (i = 0; i < 4; i++)
                {
                    int chroma = i < 2;
                    s32 frx = chroma ? chromaFrac : fx;
                    if (chroma ? midChroma : midTexel)
                        acc.c[i] = (s16)((tb.c[i] + t10.c[i] + t11.c[i] + t01.c[i] + 2) >> 2);
                    else
                    {
                        // bilinear_3tap
                        s32 sum = frx + fy;
                        s32 base = sum >= 32 ? t11.c[i] : tb.c[i];
                        s32 f0 = sum >= 32 ? 32 - fy : frx, f1 = sum >= 32 ? 32 - frx : fy;
                        s32 a = (t10.c[i] - base) * f0 + (t01.c[i] - base) * f1;
                        acc.c[i] = (s16)(((s16)(a + 0x10) >> 5) + base);
                    }
                }
            }
            else
            {
                for (i = 0; i < 4; i++)
                {
                    s32 frx = i < 2 ? chromaFrac : fx;
                    acc.c[i] = frx + fy >= 32 ? t11.c[i] : tb.c[i];
                }
            }
        }
        else
            acc = tb;
    }
    else if (midTexel)
    {
        for (i = 0; i < 4; i++) acc.c[i] = (s16)((tb.c[i] + t01.c[i] + t10.c[i] + t11.c[i] + 2) >> 2);
    }
    else if (bilerp && (sampleQuad || tlut))
    {
        s32 f0 = sumFrac >= 32 ? 32 - fy : fx, f1 = sumFrac >= 32 ? 32 - fx : fy;
        for (i = 0; i < 4; i++)
        {
            s16 a = (s16)((t10.c[i] - tb.c[i]) * f0);
            a = (s16)(a + (s16)((t01.c[i] - tb.c[i]) * f1));
            a = (s16)(a + 0x10);
            a = (s16)(a >> 5);
            acc.c[i] = (s16)(a + tb.c[i]);
        }
    }
    else
        acc = tb;

    if (!bilerp && !convertOne)
        h64_rdp_texture_convert(&acc, factors, &acc);
    *out = acc;
}

// ---- Copy mode (texture.h sample_texture_copy) ----
static s32 copy_word(H64RdpState *st, const H64RdpTile *tile, s32 s, s32 t, int sOffset, int tlut)
{
    int high = sOffset < 2, replicate = high && tile->size != 2 && !tlut;
    int sShamt = tile->size < 2 ? tile->size : 2;
    u32 idxMask = (tile->size == 3 || tlut) ? 0x3FF : 0x7FF;
    s32 samp;
    if (replicate)
    {
        s32 sA, sB, tt, samp0, samp1;
        u32 tbase, nA, nB;
        s += 2 * sOffset;
        sA = mask_s(tile, s);
        sB = mask_s(tile, s + 1);
        tt = mask_t(tile, t);
        tbase = tile->offset + tile->stride * (u32)tt;
        nA = ((tbase * 2 + ((u32)sA << sShamt)) & 0x1FFF) ^ (((u32)tt & 1) * 8);
        nB = ((tbase * 2 + ((u32)sB << sShamt)) & 0x1FFF) ^ (((u32)tt & 1) * 8);
        samp0 = (s32)tm16(st, ((nA >> 2) & idxMask) ^ 1);
        samp1 = (s32)tm16(st, ((nB >> 2) & idxMask) ^ 1);
        if (tile->size == 1)
        {
            samp0 = (samp0 >> (8 - 4 * (int)(nA & 2))) & 0xFF;
            samp1 = (samp1 >> (8 - 4 * (int)(nB & 2))) & 0xFF;
        }
        else if (tile->size == 0)
        {
            samp0 = ((samp0 >> (12 - 4 * (int)(nA & 3))) & 0xF) * 0x11;
            samp1 = ((samp1 >> (12 - 4 * (int)(nB & 3))) & 0xF) * 0x11;
        }
        else
        {
            samp0 >>= 8;
            samp1 >>= 8;
        }
        samp = (samp0 << 8) | samp1;
    }
    else
    {
        s32 sm, tt;
        u32 tbase, n;
        s += sOffset;
        sm = mask_s(tile, s);
        tt = mask_t(tile, t);
        tbase = tile->offset + tile->stride * (u32)tt;
        n = ((tbase * 2 + ((u32)sm << sShamt)) & 0x1FFF) ^ (((u32)tt & 1) * 8);
        samp = (s32)tm16(st, ((n >> 2) & idxMask) ^ 1);
        if (tlut)
        {
            if (tile->size == 0)
            {
                samp = (samp >> (12 - 4 * (int)(n & 3))) & 0xF;
                samp |= tile->palette << 4;
            }
            else
                samp = (samp >> (8 - 4 * (int)(n & 2))) & 0xFF;
            samp = (samp << 2) + sOffset;
            samp = (s32)tm16(st, ((u32)samp | 0x400) ^ 1);
        }
    }
    return samp;
}

s32 h64_rdp_sample_copy(H64RdpState *st, const H64RdpTile *tile, s32 s, s32 t, int sOffset, int tlut, int fbSize)
{
    s32 samp;
    s = shift_only(s, (s32)tile->slo, tile->shiftS) >> 5;
    t = shift_only(t, (s32)tile->tlo, tile->shiftT) >> 5;
    if (fbSize == 0) return 0;
    if (fbSize == 1)
    {
        samp = copy_word(st, tile, s, t, sOffset >> 1, tlut);
        return (samp >> (8 - 8 * (sOffset & 1))) & 0xFF;
    }
    return copy_word(st, tile, s, t, sOffset, tlut);
}

// ---- LOD (texture.h compute_lod_2cycle) ----
void h64_rdp_compute_lod(u32 *tile0, u32 *tile1, s32 *lodFrac, u32 maxLevel, s32 minLod, const s32 *st, const s32 *stDx,
                         const s32 *stDy, int perspectiveOverflow, int texLod, int sharpen, int detail)
{
    int magnify = 0, distant = 0;
    u32 tileOffset = 0;
    if (perspectiveOverflow)
    {
        distant = 1;
        *lodFrac = 0xFF;
    }
    else
    {
        s32 dxs = stDx[0] - st[0], dxt = stDx[1] - st[1], dys = stDy[0] - st[0], dyt = stDy[1] - st[1], maxD;
        dxs ^= dxs >> 31; dxt ^= dxt >> 31; dys ^= dys >> 31; dyt ^= dyt >> 31;
        maxD = dxs > dys ? dxs : dys;
        if ((dxt > dyt ? dxt : dyt) > maxD) maxD = dxt > dyt ? dxt : dyt;
        if (maxD >= 0x4000)
        {
            distant = 1;
            *lodFrac = 0xFF;
            tileOffset = maxLevel;
        }
        else if (maxD < 32)
        {
            distant = maxLevel == 0;
            magnify = 1;
            if (!sharpen && !detail) *lodFrac = distant ? 0xFF : 0;
            else *lodFrac = (s16)(((minLod > maxD ? minLod : maxD) << 3) + (sharpen ? -0x100 : 0));
        }
        else
        {
            int mipBase = h64_find_msb((u32)(maxD >> 5));
            if (mipBase < 0) mipBase = 0;
            distant = (u32)mipBase >= maxLevel;
            if (distant && !sharpen && !detail) *lodFrac = 0xFF;
            else
            {
                *lodFrac = (s16)(((maxD << 3) >> mipBase) & 0xFF);
                tileOffset = (u32)mipBase;
            }
        }
    }
    if (texLod)
    {
        if (distant) tileOffset = maxLevel;
        if (!detail)
        {
            *tile0 = (*tile0 + tileOffset) & 7;
            if (distant || (!sharpen && magnify)) *tile1 = *tile0;
            else *tile1 = (*tile0 + 1) & 7;
        }
        else
        {
            *tile1 = (*tile0 + tileOffset + ((distant || magnify) ? 1 : 2)) & 7;
            *tile0 = (*tile0 + tileOffset + (magnify ? 0 : 1)) & 7;
        }
    }
}
