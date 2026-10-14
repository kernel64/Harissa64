// Harissa64 V2 - Resident Evil 2's own RSP tasks (graphics-type tasks with
// no microcode name): the background resize, the movie frame decode and the
// movie double-buffer blend. Their microcodes ran on the LLE RSP (80-150 ms
// per frame in Xenia whenever a room or a movie starts).
//
// Ported from mupen64plus-rsp-hle's re2.c (Gilles Siberlin, GPL v2; the task
// detection by the byte sum of the microcode's first 256 bytes is from its
// hle.c): rewritten for RDRAM in N64 byte order (h64_load/store_be*).

#include "h64_hle.h"
#include "h64_hle_internal.h"
#include "../system/h64_system.h"

#define RE2_MASK 0x7FFFFFu

static u32 rd32(struct hle_t *hle, u32 a) { return h64_load_be32(hle->dram + (a & RE2_MASK & ~3u)); }
static u8 rd8(struct hle_t *hle, u32 a) { return hle->dram[a & RE2_MASK]; }

static void wr16(struct hle_t *hle, u32 a, u16 v)
{
    h64_store_be16(hle->dram + (a & RE2_MASK & ~1u), v);
    hle_dram_dirty(hle, a & RE2_MASK & ~1u, 2);
}

static void wr32(struct hle_t *hle, u32 a, u32 v)
{
    h64_store_be32(hle->dram + (a & RE2_MASK & ~3u), v);
    hle_dram_dirty(hle, a & RE2_MASK & ~3u, 4);
}

static int sat8(int x) { return x < 0 ? 0 : x > 255 ? 255 : x; }

// A 320-wide 24-bit picture (bytes blue, green, red) scaled with a bilinear
// filter into an RGBA5551 image.
static void resize_bilinear(struct hle_t *hle, u32 data)
{
    u32 src = rd32(hle, data), dst = rd32(hle, data + 4);
    s32 dstW = (s32)rd32(hle, data + 8), dstH = (s32)rd32(hle, data + 12);
    s32 xRatio = (s32)rd32(hle, data + 16), yRatio = (s32)rd32(hle, data + 20);
    u32 srcOffset = rd32(hle, data + 36);
    s64 x, y = 0;
    s32 i, j;
    src += (srcOffset >> 16) * (320 * 3);
    for (i = 0; i < dstH; i++)
    {
        s32 yr = (s32)(y >> 16);
        s64 yd = y - ((s64)yr << 16), y1 = 65536 - yd;
        x = 0;
        for (j = 0; j < dstW; j++)
        {
            s32 xr = (s32)(x >> 16), c;
            s64 xd = x - ((s64)xr << 16), x1 = 65536 - xd;
            u32 a = src + (u32)(yr * 320 + xr) * 3, rgb[3];
            for (c = 0; c < 3; c++)
            {
                s64 p = (s64)rd8(hle, a + c) * x1 * y1 + (s64)rd8(hle, a + 3 + c) * xd * y1 +
                        (s64)rd8(hle, a + 320 * 3 + c) * yd * x1 + (s64)rd8(hle, a + 320 * 3 + 3 + c) * xd * yd;
                rgb[c] = ((u32)(p >> 32) >> 3) & 0x1F;   // c = 0 blue, 1 green, 2 red
            }
            wr16(hle, dst, (u16)((rgb[2] << 11) | (rgb[1] << 6) | (rgb[0] << 1) | 1));
            dst += 2;
            x += xRatio;
        }
        y += yRatio;
    }
}

static u32 ycbcr_to_rgba(int Y, int Cb, int Cr)
{
    int r = (int)((double)Y * 0.582199097 + 0.701004028 * (double)(Cr - 128));
    int g = (int)((double)Y * 0.582199097 - 0.357070923 * (double)(Cr - 128) - 0.172073364 * (double)(Cb - 128));
    int b = (int)((double)Y * 0.582199097 + 0.886001587 * (double)(Cb - 128));
    return ((u32)sat8(r) << 24) | ((u32)sat8(g) << 16) | ((u32)sat8(b) << 8);
}

// A movie frame: luma at full size, Cb and Cr at half size, into RGBA8888.
static void decode_video_frame(struct hle_t *hle, u32 data)
{
    u32 pY = rd32(hle, data), pCb = rd32(hle, data + 4), pCr = rd32(hle, data + 8), pDst = rd32(hle, data + 12);
    s32 w = (s32)rd32(hle, data + 16), h = (s32)rd32(hle, data + 20);
    u32 step = rd32(hle, data + 36);
    s32 i, j;
    for (i = 0; i < h; i += 2)
    {
        u32 y1 = pY, y2 = pY + (u32)w, d1 = pDst, d2 = pDst + (step >> 1);
        for (j = 0; j < w; j += 2)
        {
            int cb = rd8(hle, pCb++), cr = rd8(hle, pCr++);
            wr32(hle, d1, ycbcr_to_rgba(rd8(hle, y1++), cb, cr)); d1 += 4;
            wr32(hle, d1, ycbcr_to_rgba(rd8(hle, y1++), cb, cr)); d1 += 4;
            wr32(hle, d2, ycbcr_to_rgba(rd8(hle, y2++), cb, cr)); d2 += 4;
            wr32(hle, d2, ycbcr_to_rgba(rd8(hle, y2++), cb, cr)); d2 += 4;
        }
        pY += (u32)w << 1;
        pDst += step;
    }
}

// The new movie frame averaged into the other buffer (RGBA8888).
static void fill_video_double_buffer(struct hle_t *hle, u32 data)
{
    u32 src = rd32(hle, data), dst = rd32(hle, data + 4);
    s32 w = (s32)(rd32(hle, data + 8) >> 1), h = (s32)(rd32(hle, data + 16) << 1);
    u32 stride = rd32(hle, data + 0x1C) >> 1;
    s32 i, j;
    for (i = 0; i < h; i++)
    {
        for (j = 0; j < w; j += 4)
        {
            u32 p1 = rd32(hle, src + (u32)j), p2 = rd32(hle, dst + (u32)j);
            u32 r = (((p1 >> 24) & 0xFF) + ((p2 >> 24) & 0xFF)) >> 1;
            u32 g = (((p1 >> 16) & 0xFF) + ((p2 >> 16) & 0xFF)) >> 1;
            u32 b = (((p1 >> 8) & 0xFF) + ((p2 >> 8) & 0xFF)) >> 1;
            wr32(hle, dst + (u32)j, (r << 24) | (g << 16) | (b << 8));
        }
        src += stride;
        dst += stride;
    }
}

int h64_hle_re2_task(struct hle_t *hle, u32 ucode, u32 ucodeData)
{
    u32 sum = 0, i;
    for (i = 0; i < 256; i++) sum += rd8(hle, ucode + i);
    switch (sum)
    {
    case 0x450F: resize_bilinear(hle, ucodeData); return 1;
    case 0x3B44: decode_video_frame(hle, ucodeData); return 1;
    case 0x3D84: fill_video_double_buffer(hle, ucodeData); return 1;
    }
    return 0;
}
