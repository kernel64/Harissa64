// Unit tests for the RDP texture unit: TMEM loads (LOAD_TILE, LOAD_BLOCK,
// LOAD_TLUT) and texel sampling for each format, through real RDP commands.
// Texels are written to RDRAM, loaded into TMEM, then sampled one by one
// with point sampling (bilerp on, sample quad off: the texel as stored).
#include "unit_tests.h"

#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_endian.h"
#include "../../core/rdp/h64_rdp.h"
#include "../../core/rdp/h64_rdp_state.h"
#include "../../core/system/h64_system.h"

static const u32 TEX = 0x1000;   // texture in RDRAM

static H64System *make_sys(void)
{
    H64System *sys = (H64System *)calloc(1, sizeof(H64System));
    sys->rdram = (u8 *)calloc(1, H64_RDRAM_SIZE);
    return sys;
}

static void free_sys(H64System *sys)
{
    h64_rdp_free(sys);
    free(sys->rdram);
    free(sys);
}

static void cmd(H64System *sys, u32 w0, u32 w1)
{
    u64 w = (u64)w0 << 32 | w1;
    h64_rdp_command(sys, &w, 1);
}

static void set_texture_image(H64System *sys, u32 fmt, u32 size, u32 width)
{
    cmd(sys, (0x3Du << 24) | (fmt << 21) | (size << 19) | (width - 1), TEX);
}

// line and tmem in 64-bit words
static void set_tile(H64System *sys, u32 tile, u32 fmt, u32 size, u32 line, u32 tmem, u32 palette, u32 flagsT,
                     u32 maskT, u32 shiftT, u32 flagsS, u32 maskS, u32 shiftS)
{
    cmd(sys, (0x35u << 24) | (fmt << 21) | (size << 19) | (line << 9) | tmem,
        (tile << 24) | (palette << 20) | (flagsT << 18) | (maskT << 14) | (shiftT << 10) | (flagsS << 8) | (maskS << 4) | shiftS);
}

static void load(H64System *sys, u32 op, u32 tile, u32 sl, u32 tl, u32 sh, u32 th)
{
    cmd(sys, (op << 24) | (sl << 12) | tl, (tile << 24) | (sh << 12) | th);
}

static void set_tile_size(H64System *sys, u32 tile, u32 sl, u32 tl, u32 sh, u32 th)
{
    cmd(sys, (0x32u << 24) | (sl << 12) | tl, (tile << 24) | (sh << 12) | th);
}

// Point sample of texel (s, t) on `tile`, as stored (no filter, no conversion).
static void texel(H64System *sys, u32 tile, s32 s, s32 t, int tlut, int tlutType, H64RdpTexel *out)
{
    s32 st[2], factors[4] = { 0, 0, 0, 0 };
    H64RdpTexel prev;
    memset(&prev, 0, sizeof(prev));
    st[0] = s << 5;
    st[1] = t << 5;
    h64_rdp_sample(sys->rdpState, &sys->rdpState->tiles[tile], st, tlut, tlutType, 0, 0, 0, 1, factors, &prev, out);
}

static u32 expand5(u32 v) { return (v << 3) | (v >> 2); }

void test_rdp_tmem_rgba16(H64TestContext *ctx)
{
    H64System *sys = make_sys();
    H64RdpTexel t;
    u32 s, y;
    // 8x4 RGBA16 texture, every texel distinct
    for (y = 0; y < 4; y++)
        for (s = 0; s < 8; s++)
            h64_store_be16(sys->rdram + TEX + (y * 8 + s) * 2, (u16)(((s * 3) << 11) | ((y * 5) << 6) | ((s + y) << 1) | (s & 1)));
    set_texture_image(sys, 0, 2, 8);
    set_tile(sys, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0);   // line = 8 texels * 2 bytes = 2 words
    load(sys, 0x34, 0, 0, 0, 7 << 2, 3 << 2);
    for (y = 0; y < 4; y++)   // odd lines are stored with swapped halves: sampling must undo it
        for (s = 0; s < 8; s++)
        {
            texel(sys, 0, (s32)s, (s32)y, 0, 0, &t);
            H64_CHECK_EQ(ctx, t.c[0], expand5(s * 3));
            H64_CHECK_EQ(ctx, t.c[1], expand5(y * 5));
            H64_CHECK_EQ(ctx, t.c[2], expand5(s + y));
            H64_CHECK_EQ(ctx, t.c[3], (s & 1) ? 0xFF : 0);
        }
    // The odd-line swap itself: line 1, texel 0 lives in the second 32-bit half of its word.
    H64_CHECK_EQ(ctx, h64_load_be16(sys->rdpState->tmem + 16 + 4), h64_load_be16(sys->rdram + TEX + 8 * 2));
    // The row fast path (GPU renderers) gives the same texels, from any start.
    for (y = 0; y < 4; y++)
    {
        u32 row[7], i;
        H64_CHECK(ctx, h64_rdp_fetch_row_argb(sys->rdpState, &sys->rdpState->tiles[0], 1, y, 7, 0, row));
        for (i = 0; i < 7; i++)
        {
            h64_rdp_fetch_texel(sys->rdpState, &sys->rdpState->tiles[0], 1 + i, y, 0, 0, &t);
            H64_CHECK_EQ(ctx, row[i], (u32)(t.c[3] & 0xFF) << 24 | (u32)(t.c[0] & 0xFF) << 16 | (u32)(t.c[1] & 0xFF) << 8 |
                                          (u32)(t.c[2] & 0xFF));
        }
    }
    H64_CHECK(ctx, !h64_rdp_fetch_row_argb(sys->rdpState, &sys->rdpState->tiles[0], 0, 0, 1, 1, &s));   // TLUT: not here
    free_sys(sys);
}

void test_rdp_tmem_rgba32(H64TestContext *ctx)
{
    H64System *sys = make_sys();
    H64RdpTexel t;
    u32 s, y;
    for (y = 0; y < 2; y++)
        for (s = 0; s < 4; s++)
            h64_store_be32(sys->rdram + TEX + (y * 4 + s) * 4, (u32)((s * 16 + y) << 24 | (s + 100) << 16 | (y + 200) << 8 | (s * 7)));
    set_texture_image(sys, 0, 3, 4);
    set_tile(sys, 0, 0, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0);   // 32-bit: line counted in each TMEM half (4 x 16 bits)
    load(sys, 0x34, 0, 0, 0, 3 << 2, 1 << 2);
    for (y = 0; y < 2; y++)
        for (s = 0; s < 4; s++)
        {
            texel(sys, 0, (s32)s, (s32)y, 0, 0, &t);
            H64_CHECK_EQ(ctx, t.c[0], s * 16 + y);
            H64_CHECK_EQ(ctx, t.c[1], s + 100);
            H64_CHECK_EQ(ctx, t.c[2], y + 200);
            H64_CHECK_EQ(ctx, t.c[3], s * 7);
        }
    // RG in the lower half, BA in the upper half
    H64_CHECK_EQ(ctx, sys->rdpState->tmem[0], 0);
    H64_CHECK_EQ(ctx, sys->rdpState->tmem[0x800 + 0], 200);
    free_sys(sys);
}

void test_rdp_tmem_ia_i(H64TestContext *ctx)
{
    H64System *sys = make_sys();
    H64RdpTexel t;
    u32 s;
    // IA8 (4-bit intensity, 4-bit alpha), 16 texels on one line
    for (s = 0; s < 16; s++) sys->rdram[TEX + s] = (u8)((s << 4) | (15 - s));
    set_texture_image(sys, 3, 1, 16);
    set_tile(sys, 0, 3, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0);
    load(sys, 0x34, 0, 0, 0, 15 << 2, 0);
    for (s = 0; s < 16; s++)
    {
        texel(sys, 0, (s32)s, 0, 0, 0, &t);
        H64_CHECK_EQ(ctx, t.c[0], s * 0x11);
        H64_CHECK_EQ(ctx, t.c[3], (15 - s) * 0x11);
    }
    // The same bytes as I4: two texels per byte, high nibble first
    set_tile(sys, 1, 4, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0);
    set_tile_size(sys, 1, 0, 0, 31 << 2, 0);
    for (s = 0; s < 32; s++)
    {
        u32 b = sys->rdram[TEX + s / 2], n = (s & 1) ? (b & 15) : (b >> 4);
        texel(sys, 1, (s32)s, 0, 0, 0, &t);
        H64_CHECK_EQ(ctx, t.c[0], n * 0x11);
        H64_CHECK_EQ(ctx, t.c[3], n * 0x11);
    }
    // IA4: 3-bit intensity, 1-bit alpha
    set_tile(sys, 2, 3, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0);
    set_tile_size(sys, 2, 0, 0, 31 << 2, 0);
    for (s = 0; s < 32; s++)
    {
        u32 b = sys->rdram[TEX + s / 2], n = (s & 1) ? (b & 15) : (b >> 4), i = n & 0xE;
        texel(sys, 2, (s32)s, 0, 0, 0, &t);
        H64_CHECK_EQ(ctx, t.c[0], (i << 4) | (i << 1) | (i >> 2));
        H64_CHECK_EQ(ctx, t.c[3], (n & 1) ? 0xFF : 0);
    }
    free_sys(sys);
}

void test_rdp_tmem_tlut(H64TestContext *ctx)
{
    H64System *sys = make_sys();
    H64RdpTexel t;
    u32 i, s;
    // 32-entry RGBA16 palette at TMEM 0x800 (word 256), then CI8 and CI4 textures.
    for (i = 0; i < 32; i++) h64_store_be16(sys->rdram + TEX + i * 2, (u16)((i << 11) | ((31 - i) << 6) | 1));
    set_texture_image(sys, 0, 2, 32);
    set_tile(sys, 7, 0, 2, 0, 256, 0, 0, 0, 0, 0, 0, 0);
    load(sys, 0x30, 7, 0, 0, 31 << 2, 0);
    for (i = 0; i < 4; i++)   // each entry is written to the four banks
        H64_CHECK_EQ(ctx, h64_load_be16(sys->rdpState->tmem + 0x800 + 5 * 8 + i * 2), h64_load_be16(sys->rdram + TEX + 5 * 2));

    for (s = 0; s < 16; s++) sys->rdram[TEX + 0x100 + s] = (u8)(31 - s);   // CI8 indices
    cmd(sys, (0x3Du << 24) | (2u << 21) | (1u << 19) | 15, TEX + 0x100);
    set_tile(sys, 0, 2, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0);
    load(sys, 0x34, 0, 0, 0, 15 << 2, 0);
    for (s = 0; s < 16; s++)
    {
        texel(sys, 0, (s32)s, 0, 1, 0, &t);
        H64_CHECK_EQ(ctx, t.c[0], expand5(31 - s));
        H64_CHECK_EQ(ctx, t.c[1], expand5(s));
        H64_CHECK_EQ(ctx, t.c[3], 0xFF);
    }
    // CI4 with palette 1 (entries 16..31): index n -> entry 16 + n
    set_tile(sys, 1, 2, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0);
    set_tile_size(sys, 1, 0, 0, 31 << 2, 0);
    for (s = 0; s < 32; s++)
    {
        u32 b = sys->rdram[TEX + 0x100 + s / 2], n = (s & 1) ? (b & 15) : (b >> 4);
        texel(sys, 1, (s32)s, 0, 1, 0, &t);
        H64_CHECK_EQ(ctx, t.c[0], expand5(16 + n));
    }
    // IA16 palette type
    texel(sys, 0, 3, 0, 1, 1, &t);
    {
        u32 e = h64_load_be16(sys->rdram + TEX + (31 - 3) * 2);
        H64_CHECK_EQ(ctx, t.c[0], e >> 8);
        H64_CHECK_EQ(ctx, t.c[3], e & 0xFF);
    }
    free_sys(sys);
}

void test_rdp_tmem_block(H64TestContext *ctx)
{
    H64System *sys = make_sys();
    H64RdpTexel t;
    u32 s, y;
    // 8x4 RGBA16 loaded with LOAD_BLOCK: 32 texels, dxt = 2048 / (words per line = 2) = 0x400
    for (y = 0; y < 4; y++)
        for (s = 0; s < 8; s++)
            h64_store_be16(sys->rdram + TEX + (y * 8 + s) * 2, (u16)(((s + 8 * y) << 1) | 1));
    set_texture_image(sys, 0, 2, 8);
    // As libultra does it: a load tile with line 0 (LOAD_BLOCK adds line * T to each word), then the render tile.
    set_tile(sys, 7, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    load(sys, 0x33, 7, 0, 0, 31, 0x400);
    set_tile(sys, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0);
    set_tile_size(sys, 0, 0, 0, 7 << 2, 3 << 2);
    for (y = 0; y < 4; y++)
        for (s = 0; s < 8; s++)
        {
            texel(sys, 0, (s32)s, (s32)y, 0, 0, &t);
            H64_CHECK_EQ(ctx, t.c[2], expand5(s + 8 * y));
        }
    free_sys(sys);
}

void test_rdp_tmem_wrap(H64TestContext *ctx)
{
    H64System *sys = make_sys();
    H64RdpTexel t;
    u32 s;
    for (s = 0; s < 8; s++) h64_store_be16(sys->rdram + TEX + s * 2, (u16)((s + 1) << 1));
    set_texture_image(sys, 0, 2, 8);
    set_tile(sys, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0);
    load(sys, 0x34, 0, 0, 0, 7 << 2, 0);
    // wrap with mask 2 (4 texels): s = 5 -> 1
    set_tile(sys, 1, 0, 2, 2, 0, 0, 0, 0, 0, 0, 2, 0);
    set_tile_size(sys, 1, 0, 0, 7 << 2, 0);
    texel(sys, 1, 5, 0, 0, 0, &t);
    H64_CHECK_EQ(ctx, t.c[2], expand5(2));
    // mirror with mask 2: s = 4..7 read 3..0
    set_tile(sys, 2, 0, 2, 2, 0, 0, 0, 0, 0, 1, 2, 0);
    set_tile_size(sys, 2, 0, 0, 7 << 2, 0);
    for (s = 4; s < 8; s++)
    {
        texel(sys, 2, (s32)s, 0, 0, 0, &t);
        H64_CHECK_EQ(ctx, t.c[2], expand5(8 - s));
    }
    // clamp (cs = 2, mask 0): beyond SH the last texel is used
    set_tile(sys, 3, 0, 2, 2, 0, 0, 0, 0, 0, 2, 0, 0);
    set_tile_size(sys, 3, 0, 0, 3 << 2, 0);
    texel(sys, 3, 6, 0, 0, 0, &t);
    H64_CHECK_EQ(ctx, t.c[2], expand5(4));
    // shift S right by 1: s = 6 reads texel 3
    set_tile(sys, 4, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 1);
    set_tile_size(sys, 4, 0, 0, 7 << 2, 0);
    texel(sys, 4, 6, 0, 0, 0, &t);
    H64_CHECK_EQ(ctx, t.c[2], expand5(4));
    free_sys(sys);
}

void test_rdp_tmem_yuv(H64TestContext *ctx)
{
    H64System *sys = make_sys();
    H64RdpTexel t;
    u32 p;
    // 4 pixels as U Y0 V Y1 pairs
    for (p = 0; p < 2; p++)
    {
        u8 *d = sys->rdram + TEX + p * 4;
        d[0] = (u8)(0x90 + p); d[1] = (u8)(10 + 2 * p); d[2] = (u8)(0x70 - p); d[3] = (u8)(11 + 2 * p);
    }
    set_texture_image(sys, 1, 2, 4);
    set_tile(sys, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0);
    load(sys, 0x34, 0, 0, 0, 3 << 2, 0);
    for (p = 0; p < 4; p++)
    {
        texel(sys, 0, (s32)p, 0, 0, 0, &t);
        H64_CHECK_EQ(ctx, (u32)(t.c[0] & 0xFFFF), (u32)((0x90 + p / 2 - 0x80) & 0xFFFF));
        H64_CHECK_EQ(ctx, (u32)(t.c[1] & 0xFFFF), (u32)((0x70 - p / 2 - 0x80) & 0xFFFF));
        H64_CHECK_EQ(ctx, t.c[2], 10 + p);
    }
    free_sys(sys);
}
