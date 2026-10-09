// Harissa64 V2 - ROMs inside .zip archives (see h64_zip.h).
// Zip layout from PKWARE's APPNOTE.TXT (end of central directory, central
// directory and local headers); inflate is zlib's (third_party/zlib).
#include "h64_zip.h"

#include <stdlib.h>
#include <string.h>

#include "../../third_party/zlib/zlib.h"

static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

u32 h64_zip_mem_read(void *user, u32 offset, void *buf, u32 len)
{
    const H64ZipMem *m = (const H64ZipMem *)user;
    if (offset >= m->size) return 0;
    if (len > m->size - offset) len = m->size - offset;
    memcpy(buf, m->data + offset, len);
    return len;
}

static int is_rom_word(const u8 *p)
{
    u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    return w == 0x80371240u || w == 0x37804012u || w == 0x40123780u;   // z64, v64 (byte-swapped), n64 (word-swapped)
}

static int has_rom_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    static const char *exts[] = { ".z64", ".n64", ".v64", ".rom", ".u64" };
    unsigned i;
    if (!dot) return 0;
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
    {
        const char *a = dot, *b = exts[i];
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

static voidpf zalloc_fn(voidpf, uInt items, uInt size) { return malloc((size_t)items * size); }
static void zfree_fn(voidpf, voidpf p) { free(p); }

int h64_zip_extract(const H64ZipReader *r, const H64ZipEntry *e, u8 *out, u32 len)
{
    u8 lh[30];
    u32 data;
    if (len > e->size) return -1;
    if (r->read(r->user, e->localOffset, lh, 30) != 30 || le32(lh) != 0x04034B50u) return -1;
    data = e->localOffset + 30 + le16(lh + 26) + le16(lh + 28);
    if (e->method == 0)
    {
        if (r->read(r->user, data, out, len) != len) return -1;
    }
    else if (e->method == 8)
    {
        // Streamed: compressed data read in chunks, inflated until `len` bytes are out.
        const u32 chunk = 256 * 1024;
        u8 *in = (u8 *)malloc(chunk);
        z_stream z;
        u32 inPos = 0;
        int ret = Z_OK;
        if (!in) return -1;
        memset(&z, 0, sizeof(z));
        z.zalloc = zalloc_fn;
        z.zfree = zfree_fn;
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) { free(in); return -1; }   // raw deflate
        z.next_out = out;
        z.avail_out = len;
        while (z.avail_out > 0 && ret != Z_STREAM_END)
        {
            if (z.avail_in == 0)
            {
                u32 n = e->compSize - inPos < chunk ? e->compSize - inPos : chunk;
                if (n == 0 || r->read(r->user, data + inPos, in, n) != n) break;
                inPos += n;
                z.next_in = in;
                z.avail_in = n;
            }
            ret = inflate(&z, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) break;
        }
        inflateEnd(&z);
        free(in);
        if (z.avail_out != 0) return -1;
    }
    else
        return -1;
    if (len == e->size && crc32(0L, out, len) != e->crc) return -1;
    return 0;
}

int h64_zip_find_rom(const H64ZipReader *r, H64ZipEntry *e)
{
    u8 *tail, *cd;
    u32 tailLen = r->size < 65557 ? r->size : 65557, tailOff = r->size - tailLen, i, eocd = 0, found = 0;
    u32 count, cdSize, cdOff, pos, pass;
    if (r->size < 22) return -1;
    tail = (u8 *)malloc(tailLen);
    if (!tail) return -1;
    if (r->read(r->user, tailOff, tail, tailLen) != tailLen) { free(tail); return -1; }
    for (i = tailLen - 22 + 1; i-- > 0;)
        if (le32(tail + i) == 0x06054B50u) { eocd = i; found = 1; break; }
    if (!found) { free(tail); return -1; }
    count = le16(tail + eocd + 10);
    cdSize = le32(tail + eocd + 12);
    cdOff = le32(tail + eocd + 16);
    free(tail);
    if (cdOff > r->size || cdSize > r->size - cdOff || cdSize > 16u * 1024 * 1024) return -1;
    cd = (u8 *)malloc(cdSize ? cdSize : 1);
    if (!cd) return -1;
    if (r->read(r->user, cdOff, cd, cdSize) != cdSize) { free(cd); return -1; }
    // Pass 0: entries named like ROMs; pass 1: any entry. Each candidate's
    // first word must be an N64 ROM's (any dump order).
    for (pass = 0; pass < 2; pass++)
    {
        pos = 0;
        for (i = 0; i < count && pos + 46 <= cdSize && le32(cd + pos) == 0x02014B50u; i++)
        {
            u32 nameLen = le16(cd + pos + 28), extraLen = le16(cd + pos + 30), commentLen = le16(cd + pos + 32);
            u32 flags = le16(cd + pos + 8);
            H64ZipEntry c;
            u8 head[4];
            if (pos + 46 + nameLen > cdSize) break;
            memset(&c, 0, sizeof(c));
            memcpy(c.name, cd + pos + 46, nameLen < sizeof(c.name) - 1 ? nameLen : sizeof(c.name) - 1);
            c.method = le16(cd + pos + 10);
            c.crc = le32(cd + pos + 16);
            c.compSize = le32(cd + pos + 20);
            c.size = le32(cd + pos + 24);
            c.localOffset = le32(cd + pos + 42);
            pos += 46 + nameLen + extraLen + commentLen;
            if ((flags & 1) || c.size < 0x1000 || (c.method != 0 && c.method != 8)) continue;   // encrypted, too small
            if (pass == 0 && !has_rom_extension(c.name)) continue;
            if (h64_zip_extract(r, &c, head, 4) == 0 && is_rom_word(head))
            {
                *e = c;
                free(cd);
                return 0;
            }
        }
    }
    free(cd);
    return -1;
}
