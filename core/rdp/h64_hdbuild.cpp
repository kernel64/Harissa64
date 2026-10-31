// Harissa64 V2 - Harissa64 HD files from community texture packs (h64_hdbuild.h).
#include "h64_hdbuild.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

// stb (third_party/stb, public domain / MIT): PNG only, from memory, no SIMD.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x)
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4456 4457 4701 4100 4127)
#pragma warning(disable : 4505)   // (reported at the end of the file: not inside the push)
#endif
#include "../../third_party/stb/stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "../../third_party/stb/stb_dxt.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#define HD_FILE_MAX 0x7FF00000u   // the reader seeks with a long: stay under 2 GB

namespace {

int upper_eq(const char *a, size_t an, const char *b)
{
    size_t i;
    for (i = 0; i < an; i++)
    {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (!cb) return 0;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return b[an] == 0;
}

// Half size (box filter); w, h even.
u8 *half(const u8 *src, int w, int h)
{
    int x, y, c, hw = w / 2, hh = h / 2;
    u8 *d = (u8 *)malloc((size_t)hw * hh * 4);
    if (!d) return NULL;
    for (y = 0; y < hh; y++)
        for (x = 0; x < hw; x++)
            for (c = 0; c < 4; c++)
            {
                const u8 *s0 = src + ((size_t)(2 * y) * w + 2 * x) * 4 + c, *s1 = s0 + (size_t)w * 4;
                d[((size_t)y * hw + x) * 4 + c] = (u8)((s0[0] + s0[4] + s1[0] + s1[4] + 2) / 4);
            }
    return d;
}

// One level's DXT blocks, 16-bit words byte-swapped; returns the bytes written.
u32 compress(const u8 *rgba, int w, int h, int dxt5, u8 *out)
{
    unsigned char block[64], dst[16];
    int bx, by, x, y, i, n = dxt5 ? 16 : 8;
    u32 pos = 0;
    for (by = 0; by < h; by += 4)
        for (bx = 0; bx < w; bx += 4)
        {
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    memcpy(block + (y * 4 + x) * 4, rgba + ((size_t)(by + y) * w + bx + x) * 4, 4);
            stb_compress_dxt_block(dst, block, dxt5, STB_DXT_HIGHQUAL);
            for (i = 0; i < n; i += 2)
            {
                out[pos++] = dst[i + 1];
                out[pos++] = dst[i];
            }
        }
    return pos;
}

void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

bool key_less(const H64HdEntry &a, const H64HdEntry &b)
{
    if (a.texCrc != b.texCrc) return a.texCrc < b.texCrc;
    if (a.palCrc != b.palCrc) return a.palCrc < b.palCrc;
    if (a.fmt != b.fmt) return a.fmt < b.fmt;
    if (a.size != b.size) return a.size < b.size;
    if (a.flags != b.flags) return a.flags < b.flags;
    return a.pad2 < b.pad2;   // (the caller's order: the first picture first)
}

bool key_eq(const H64HdEntry &a, const H64HdEntry &b)
{
    return a.texCrc == b.texCrc && a.palCrc == b.palCrc && a.fmt == b.fmt && a.size == b.size && a.flags == b.flags;
}

}  // namespace

struct H64HdBuilder
{
    FILE *f;
    char path[260];
    u32 capacity, dataOff, pos;
    std::vector<H64HdEntry> index;
};

int h64_hdbuild_parse(const char *fileName, const char *romName, H64HdEntry *e)
{
    // NAME#TEX#FMT#SIZE[#PAL]_suffix.png (the suffix: _all, _rgb, _ciByRGBA...).
    const char *p[6];
    size_t n[6];
    int parts = 0;
    const char *s = fileName, *end = fileName + strlen(fileName), *dot, *us;
    char buf[16];
    dot = strrchr(fileName, '.');
    if (dot) end = dot;
    us = NULL;
    {
        const char *q;
        for (q = fileName; q < end; q++)
            if (*q == '_') us = q;
            else if (*q == '#') us = NULL;   // an underscore before the last '#' is part of the name
    }
    if (us) end = us;
    while (parts < 6)
    {
        const char *h = s;
        while (h < end && *h != '#') h++;
        p[parts] = s;
        n[parts] = (size_t)(h - s);
        parts++;
        if (h >= end) break;
        s = h + 1;
    }
    if (parts < 4 || !upper_eq(p[0], n[0], romName)) return 0;
    memset(e, 0, sizeof(*e));
    if (n[1] == 1 && p[1][0] == '$') e->flags |= H64HD_WILDCARD;
    else
    {
        if (!n[1] || n[1] > 8) return 0;
        memcpy(buf, p[1], n[1]);
        buf[n[1]] = 0;
        e->texCrc = (u32)strtoul(buf, NULL, 16);
    }
    if (!n[2] || n[2] > 2 || !n[3] || n[3] > 2) return 0;
    memcpy(buf, p[2], n[2]); buf[n[2]] = 0; e->fmt = (u8)atoi(buf);
    memcpy(buf, p[3], n[3]); buf[n[3]] = 0; e->size = (u8)atoi(buf);
    if (parts > 4 && n[4] && n[4] <= 8)
    {
        memcpy(buf, p[4], n[4]);
        buf[n[4]] = 0;
        e->palCrc = (u32)strtoul(buf, NULL, 16);
        e->flags |= H64HD_HAS_PAL;
    }
    if ((e->flags & H64HD_WILDCARD) && !(e->flags & H64HD_HAS_PAL)) return 0;
    return 1;
}

int h64_hdbuild_skip(const char *path)
{
    static const char *skips[] = { "Hacks/", "iQue/", "Ports/", "rt64/", "Hacks\\", "iQue\\", "Ports\\", "rt64\\" };
    size_t i;
    for (i = 0; i < sizeof(skips) / sizeof(skips[0]); i++)
        if (strstr(path, skips[i])) return 1;
    return 0;
}

int h64_hdbuild_make(const u8 *png, u32 len, int maxSide, H64HdItem *item)
{
    int w, h, comp, pw, ph, alpha = 0, levels = 0, lw, lh;
    size_t i;
    u8 *img, *lv;
    u32 total = 0, pos = 0;
    item->data = NULL;
    item->bytes = 0;
    img = stbi_load_from_memory(png, (int)len, &w, &h, &comp, 4);
    if (!img || w <= 0 || h <= 0) { if (img) stbi_image_free(img); return -1; }
    // The size limit (halving keeps the aspect), then a multiple of 4 by repeating the edge.
    while ((w > maxSide || h > maxSide) && w % 2 == 0 && h % 2 == 0)
    {
        u8 *d = half(img, w, h);
        stbi_image_free(img);
        if (!d) return -1;
        img = d;
        w /= 2;
        h /= 2;
    }
    pw = (w + 3) & ~3;
    ph = (h + 3) & ~3;
    if (pw != w || ph != h)
    {
        int x, y;
        u8 *p = (u8 *)malloc((size_t)pw * ph * 4);
        if (!p) { stbi_image_free(img); return -1; }
        for (y = 0; y < ph; y++)
            for (x = 0; x < pw; x++)
                memcpy(p + ((size_t)y * pw + x) * 4, img + ((size_t)(y < h ? y : h - 1) * w + (x < w ? x : w - 1)) * 4, 4);
        stbi_image_free(img);
        img = p;
        w = pw;
        h = ph;
    }
    for (i = 3; i < (size_t)w * h * 4; i += 4)
        if (img[i] < 250) { alpha = 1; break; }
    // The levels' total size, then the levels.
    for (lw = w, lh = h;;)
    {
        total += (u32)((lw / 4) * (lh / 4) * (alpha ? 16 : 8));
        levels++;
        if (lw % 8 || lh % 8 || levels >= 12) break;
        lw /= 2;
        lh /= 2;
    }
    item->data = (u8 *)malloc(total);
    if (!item->data) { stbi_image_free(img); return -1; }
    lv = img;
    lw = w;
    lh = h;
    for (comp = 0; comp < levels; comp++)
    {
        pos += compress(lv, lw, lh, alpha, item->data + pos);
        if (comp + 1 < levels)
        {
            u8 *d = half(lv, lw, lh);
            if (lv == img) stbi_image_free(lv);
            else free(lv);
            lv = d;
            if (!lv) { free(item->data); item->data = NULL; return -1; }
            lw /= 2;
            lh /= 2;
        }
    }
    if (lv == img) stbi_image_free(lv);
    else free(lv);
    item->bytes = pos;
    item->e.dxt = (u8)(alpha ? 5 : 1);
    item->e.width = (u16)w;
    item->e.height = (u16)h;
    item->e.mips = (u8)levels;
    return 0;
}

H64HdBuilder *h64_hdbuild_begin(const char *path, u32 capacity)
{
    H64HdBuilder *b;
    u8 zeros[512];
    u32 left;
    if (capacity > 1000000) return NULL;
    b = new H64HdBuilder;
    b->f = fopen(path, "wb");
    if (!b->f) { delete b; return NULL; }
    strncpy(b->path, path, sizeof(b->path) - 1);
    b->path[sizeof(b->path) - 1] = 0;
    b->capacity = capacity;
    b->dataOff = (H64HD_HEADER_BYTES + capacity * H64HD_ENTRY_BYTES + 127) & ~127u;
    // Header and index are written at the end: zeros for now.
    memset(zeros, 0, sizeof(zeros));
    for (left = b->dataOff; left;)
    {
        u32 n = left < sizeof(zeros) ? left : (u32)sizeof(zeros);
        if (fwrite(zeros, 1, n, b->f) != n) { h64_hdbuild_abort(b); return NULL; }
        left -= n;
    }
    b->pos = b->dataOff;
    return b;
}

int h64_hdbuild_add(H64HdBuilder *b, H64HdItem *item)
{
    static const u8 zeros[128] = { 0 };
    u32 padded = (item->bytes + 127) & ~127u;
    int ok = b->index.size() < b->capacity && item->data && (u64)b->pos + padded <= HD_FILE_MAX &&
             fwrite(item->data, 1, item->bytes, b->f) == item->bytes &&
             (padded == item->bytes || fwrite(zeros, 1, padded - item->bytes, b->f) == padded - item->bytes);
    free(item->data);
    item->data = NULL;
    if (!ok) return -1;
    item->e.offset = b->pos;
    item->e.bytes = item->bytes;
    item->e.pad0 = 0;
    item->e.pad1 = 0;
    b->index.push_back(item->e);
    b->pos += padded;
    return 0;
}

int h64_hdbuild_end(H64HdBuilder *b, u32 *count, u32 *repeated, u32 *bytes)
{
    std::vector<H64HdEntry> &v = b->index;
    std::vector<u8> head;
    size_t i, kept = 0;
    int ok;
    std::sort(v.begin(), v.end(), key_less);
    for (i = 0; i < v.size(); i++)
        if (!kept || !key_eq(v[kept - 1], v[i])) v[kept++] = v[i];
    if (repeated) *repeated = (u32)(v.size() - kept);
    v.resize(kept);
    head.resize(H64HD_HEADER_BYTES + kept * H64HD_ENTRY_BYTES);
    memcpy(&head[0], H64HD_MAGIC, 8);
    put32(&head[8], H64HD_VERSION);
    put32(&head[12], (u32)kept);
    put32(&head[16], H64HD_HEADER_BYTES);
    put32(&head[20], b->dataOff);
    put32(&head[24], 0);
    put32(&head[28], 0);
    for (i = 0; i < kept; i++)
    {
        u8 *p = &head[H64HD_HEADER_BYTES + i * H64HD_ENTRY_BYTES];
        const H64HdEntry &e = v[i];
        put32(p, e.texCrc);
        put32(p + 4, e.palCrc);
        p[8] = e.fmt; p[9] = e.size; p[10] = e.flags; p[11] = e.dxt;
        put16(p + 12, e.width);
        put16(p + 14, e.height);
        p[16] = e.mips; p[17] = 0;
        put16(p + 18, 0);
        put32(p + 20, e.offset);
        put32(p + 24, e.bytes);
        put32(p + 28, 0);
    }
    ok = fseek(b->f, 0, SEEK_SET) == 0 && fwrite(&head[0], 1, head.size(), b->f) == head.size();
    ok = (fclose(b->f) == 0) && ok;
    b->f = NULL;
    if (count) *count = (u32)kept;
    if (bytes) *bytes = b->pos;
    if (!ok) remove(b->path);
    delete b;
    return ok ? 0 : -1;
}

void h64_hdbuild_abort(H64HdBuilder *b)
{
    if (!b) return;
    if (b->f) fclose(b->f);
    remove(b->path);
    delete b;
}
