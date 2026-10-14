// Harissa64 V2 - PNG decoding and image downscaling (see h64_png.h).
// Written from the PNG specification (W3C, second edition): chunk layout,
// filters 0-4 with the Paeth predictor, Adam7 passes, sample scaling.
#include "h64_png.h"

#include <stdlib.h>
#include <string.h>

#include "../../third_party/zlib/zlib.h"

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static voidpf png_zalloc(voidpf, uInt items, uInt size) { return malloc((size_t)items * size); }
static void png_zfree(voidpf, voidpf p) { free(p); }

struct PngHeader
{
    u32 w, h;
    int depth, ctype, interlace, channels, bitsPP;
    u8 pal[256][4];
    int palCount;
    int hasKey;          // tRNS for grey / RGB: this colour is transparent
    u32 key[3];
};

static const int s_adamX0[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const int s_adamY0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const int s_adamDX[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const int s_adamDY[7] = { 8, 8, 8, 4, 4, 2, 2 };

// Pixels of one pass (or of the whole image when not interlaced) along an axis.
static u32 pass_size(u32 full, int start, int step) { return full > (u32)start ? (full - start + step - 1) / step : 0; }

static u32 row_bytes(const PngHeader *hd, u32 w) { return (u32)(((u64)w * hd->bitsPP + 7) / 8); }

// Sample `i` of a row, at the image's bit depth (0..2^depth-1).
static u32 sample(const u8 *row, u32 i, int depth)
{
    switch (depth)
    {
    case 8: return row[i];
    case 16: return ((u32)row[i * 2] << 8) | row[i * 2 + 1];
    default:
    {
        u32 bit = i * depth;
        return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    }
    }
}

static u8 to8(u32 v, int depth)
{
    if (depth == 8) return (u8)v;
    if (depth == 16) return (u8)(v >> 8);
    return (u8)(v * 255 / ((1u << depth) - 1));
}

static void put_row(const PngHeader *hd, const u8 *row, u32 n, u8 *out, u32 outW, u32 y, u32 x0, u32 dx)
{
    u32 i;
    for (i = 0; i < n; i++)
    {
        u8 *px = out + ((size_t)y * outW + x0 + (size_t)i * dx) * 4;
        int c = hd->channels;
        u32 s0 = sample(row, i * c, hd->depth);
        switch (hd->ctype)
        {
        case 0:   // grey
            px[0] = px[1] = px[2] = to8(s0, hd->depth);
            px[3] = hd->hasKey && s0 == hd->key[0] ? 0 : 255;
            break;
        case 2:   // RGB
        {
            u32 s1 = sample(row, i * c + 1, hd->depth), s2 = sample(row, i * c + 2, hd->depth);
            px[0] = to8(s0, hd->depth);
            px[1] = to8(s1, hd->depth);
            px[2] = to8(s2, hd->depth);
            px[3] = hd->hasKey && s0 == hd->key[0] && s1 == hd->key[1] && s2 == hd->key[2] ? 0 : 255;
            break;
        }
        case 3:   // palette (an index past the palette reads black)
        {
            const u8 *p = s0 < (u32)hd->palCount ? hd->pal[s0] : hd->pal[0];
            if (s0 >= (u32)hd->palCount) { px[0] = px[1] = px[2] = 0; px[3] = 255; }
            else { px[0] = p[0]; px[1] = p[1]; px[2] = p[2]; px[3] = p[3]; }
            break;
        }
        case 4:   // grey + alpha
            px[0] = px[1] = px[2] = to8(s0, hd->depth);
            px[3] = to8(sample(row, i * c + 1, hd->depth), hd->depth);
            break;
        default:  // 6: RGBA
            px[0] = to8(s0, hd->depth);
            px[1] = to8(sample(row, i * c + 1, hd->depth), hd->depth);
            px[2] = to8(sample(row, i * c + 2, hd->depth), hd->depth);
            px[3] = to8(sample(row, i * c + 3, hd->depth), hd->depth);
            break;
        }
    }
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// Undoes the filter of one row in place (`prev`: the previous row of the pass, or NULL).
static int unfilter(u8 *row, const u8 *prev, u32 len, int filter, u32 bpp)
{
    u32 i;
    switch (filter)
    {
    case 0: return 1;
    case 1:
        for (i = bpp; i < len; i++) row[i] = (u8)(row[i] + row[i - bpp]);
        return 1;
    case 2:
        if (prev)
            for (i = 0; i < len; i++) row[i] = (u8)(row[i] + prev[i]);
        return 1;
    case 3:
        for (i = 0; i < len; i++)
        {
            int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0;
            row[i] = (u8)(row[i] + ((a + b) >> 1));
        }
        return 1;
    case 4:
        for (i = 0; i < len; i++)
        {
            int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
            row[i] = (u8)(row[i] + paeth(a, b, c));
        }
        return 1;
    }
    return 0;
}

static int valid_depth(int ctype, int depth)
{
    switch (ctype)
    {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2: case 4: case 6: return depth == 8 || depth == 16;
    }
    return 0;
}

u8 *h64_png_decode(const u8 *data, u32 size, int *wOut, int *hOut, u32 maxPixels)
{
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    PngHeader hd;
    u32 pos = 8, idatLen = 0, rawLen = 0, p;
    u8 *idat = NULL, *raw = NULL, *out = NULL;
    int gotHeader = 0, pass, ok = 0;
    z_stream z;

    *wOut = *hOut = 0;
    if (!data || size < 8 + 25 || memcmp(data, sig, 8)) return NULL;
    memset(&hd, 0, sizeof(hd));
    for (p = 0; p < 256; p++) hd.pal[p][3] = 255;
    idat = (u8 *)malloc(size);
    if (!idat) return NULL;

    // Chunks: the header, the palette and transparency, the image data.
    while (pos + 12 <= size)
    {
        u32 len = be32(data + pos);
        const u8 *type = data + pos + 4, *d = data + pos + 8;
        if (len > size - pos - 12) goto done;
        if (!memcmp(type, "IHDR", 4))
        {
            if (len < 13 || gotHeader) goto done;
            hd.w = be32(d);
            hd.h = be32(d + 4);
            hd.depth = d[8];
            hd.ctype = d[9];
            hd.interlace = d[12];
            if (!hd.w || !hd.h || hd.w > 16384 || hd.h > 16384 || (u64)hd.w * hd.h > maxPixels) goto done;
            if (!valid_depth(hd.ctype, hd.depth) || d[10] != 0 || d[11] != 0 || hd.interlace > 1) goto done;
            hd.channels = hd.ctype == 2 ? 3 : hd.ctype == 4 ? 2 : hd.ctype == 6 ? 4 : 1;
            hd.bitsPP = hd.channels * hd.depth;
            gotHeader = 1;
        }
        else if (!gotHeader)
            goto done;   // IHDR must come first
        else if (!memcmp(type, "PLTE", 4))
        {
            u32 i;
            if (len % 3 || len > 768) goto done;
            hd.palCount = (int)(len / 3);
            for (i = 0; i < len / 3; i++)
            {
                hd.pal[i][0] = d[i * 3];
                hd.pal[i][1] = d[i * 3 + 1];
                hd.pal[i][2] = d[i * 3 + 2];
            }
        }
        else if (!memcmp(type, "tRNS", 4))
        {
            u32 i;
            if (hd.ctype == 3)
                for (i = 0; i < len && i < 256; i++) hd.pal[i][3] = d[i];
            else if (hd.ctype == 0 && len >= 2)
            {
                hd.hasKey = 1;
                hd.key[0] = ((u32)d[0] << 8) | d[1];
            }
            else if (hd.ctype == 2 && len >= 6)
            {
                hd.hasKey = 1;
                for (i = 0; i < 3; i++) hd.key[i] = ((u32)d[i * 2] << 8) | d[i * 2 + 1];
            }
        }
        else if (!memcmp(type, "IDAT", 4))
        {
            memcpy(idat + idatLen, d, len);
            idatLen += len;
        }
        else if (!memcmp(type, "IEND", 4))
            break;
        else if (!(type[0] & 0x20))
            goto done;   // an unknown critical chunk
        pos += 12 + len;
    }
    if (!gotHeader || !idatLen || (hd.ctype == 3 && !hd.palCount)) goto done;

    // The filtered rows of every pass, one filter byte each.
    for (pass = hd.interlace ? 0 : 6; pass < 7; pass++)
    {
        u32 pw = hd.interlace ? pass_size(hd.w, s_adamX0[pass], s_adamDX[pass]) : hd.w;
        u32 ph = hd.interlace ? pass_size(hd.h, s_adamY0[pass], s_adamDY[pass]) : hd.h;
        if (pw && ph) rawLen += ph * (row_bytes(&hd, pw) + 1);
    }
    raw = (u8 *)malloc(rawLen);
    out = (u8 *)malloc((size_t)hd.w * hd.h * 4);
    if (!raw || !out) goto done;
    memset(&z, 0, sizeof(z));
    z.zalloc = png_zalloc;
    z.zfree = png_zfree;
    if (inflateInit(&z) != Z_OK) goto done;
    z.next_in = idat;
    z.avail_in = idatLen;
    z.next_out = raw;
    z.avail_out = rawLen;
    {
        int r = inflate(&z, Z_FINISH);
        u32 left = z.avail_out;
        inflateEnd(&z);
        if ((r != Z_STREAM_END && r != Z_BUF_ERROR && r != Z_OK) || left) goto done;
    }

    // Unfilter and convert, pass by pass.
    {
        u32 off = 0, bpp = (u32)(hd.bitsPP >= 8 ? hd.bitsPP / 8 : 1);
        for (pass = hd.interlace ? 0 : 6; pass < 7; pass++)
        {
            u32 x0 = hd.interlace ? s_adamX0[pass] : 0, y0 = hd.interlace ? s_adamY0[pass] : 0;
            u32 dx = hd.interlace ? s_adamDX[pass] : 1, dy = hd.interlace ? s_adamDY[pass] : 1;
            u32 pw = hd.interlace ? pass_size(hd.w, x0, dx) : hd.w, ph = hd.interlace ? pass_size(hd.h, y0, dy) : hd.h;
            u32 rb = row_bytes(&hd, pw), y;
            const u8 *prev = NULL;
            if (!pw || !ph) continue;
            for (y = 0; y < ph; y++)
            {
                u8 *row = raw + off + 1;
                if (!unfilter(row, prev, rb, raw[off], bpp)) goto done;
                put_row(&hd, row, pw, out, hd.w, y0 + y * dy, x0, dx);
                prev = row;
                off += rb + 1;
            }
        }
    }
    ok = 1;

done:
    free(idat);
    free(raw);
    if (!ok)
    {
        free(out);
        return NULL;
    }
    *wOut = (int)hd.w;
    *hOut = (int)hd.h;
    return out;
}

void h64_image_fit(int w, int h, int maxW, int maxH, int *outW, int *outH)
{
    int dw = w, dh = h;
    if (w <= 0 || h <= 0) { *outW = *outH = 1; return; }
    if (dw > maxW) { dw = maxW; dh = (int)((s64)h * maxW / w); }
    if (dh > maxH) { dh = maxH; dw = (int)((s64)w * maxH / h); }
    *outW = dw < 1 ? 1 : dw;
    *outH = dh < 1 ? 1 : dh;
}

u8 *h64_image_scale(const u8 *src, int sw, int sh, int dw, int dh)
{
    u8 *dst;
    int x, y;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return NULL;
    dst = (u8 *)malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;
    for (y = 0; y < dh; y++)
    {
        // Source rows [ya, yb) and columns [xa, xb) of this pixel (at least one each).
        int ya = (int)((s64)y * sh / dh), yb = (int)((s64)(y + 1) * sh / dh);
        if (yb <= ya) yb = ya + 1;
        for (x = 0; x < dw; x++)
        {
            int xa = (int)((s64)x * sw / dw), xb = (int)((s64)(x + 1) * sw / dw), i, j;
            u32 r = 0, g = 0, b = 0, a = 0, n;
            u8 *d = dst + ((size_t)y * dw + x) * 4;
            if (xb <= xa) xb = xa + 1;
            n = (u32)((xb - xa) * (yb - ya));
            for (j = ya; j < yb; j++)
            {
                const u8 *s = src + ((size_t)j * sw + xa) * 4;
                for (i = xa; i < xb; i++, s += 4)
                {
                    r += s[0] * s[3];
                    g += s[1] * s[3];
                    b += s[2] * s[3];
                    a += s[3];
                }
            }
            if (a)
            {
                d[0] = (u8)(r / a);
                d[1] = (u8)(g / a);
                d[2] = (u8)(b / a);
            }
            else
                d[0] = d[1] = d[2] = 0;
            d[3] = (u8)(a / n);
        }
    }
    return dst;
}
