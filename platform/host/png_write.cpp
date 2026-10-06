// Minimal PNG writer for h64test screenshots: RGB8, deflate "stored"
// blocks (no compression), so it needs no zlib.
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_crc32.h"

static void put32(FILE *f, u32 v)
{
    u8 b[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v };
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const u8 *data, u32 len)
{
    u32 crc;
    put32(f, len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    crc = h64_crc32(0, (const u8 *)type, 4);
    crc = h64_crc32(crc, data, len);
    put32(f, crc);
}

int h64_png_write_rgb(const char *path, const u8 *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    u32 rawLen = (u32)h * ((u32)w * 3 + 1), blocks = (rawLen + 65534) / 65535, zLen = 2 + rawLen + blocks * 5 + 4;
    u8 *raw, *z, ihdr[13];
    u32 i, pos, a = 1, b = 0;
    int y;
    if (!f) return -1;
    raw = (u8 *)malloc(rawLen);
    z = (u8 *)malloc(zLen);
    for (y = 0; y < h; y++)
    {
        raw[(u32)y * ((u32)w * 3 + 1)] = 0;   // filter: none
        memcpy(raw + (u32)y * ((u32)w * 3 + 1) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    pos = 0;
    z[pos++] = 0x78; z[pos++] = 0x01;
    for (i = 0; i < rawLen; i += 65535)
    {
        u32 n = rawLen - i < 65535 ? rawLen - i : 65535;
        z[pos++] = (u8)(i + n >= rawLen ? 1 : 0);
        z[pos++] = (u8)n; z[pos++] = (u8)(n >> 8);
        z[pos++] = (u8)~n; z[pos++] = (u8)(~n >> 8);
        memcpy(z + pos, raw + i, n);
        pos += n;
    }
    for (i = 0; i < rawLen; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }   // Adler-32
    z[pos++] = (u8)(b >> 8); z[pos++] = (u8)b; z[pos++] = (u8)(a >> 8); z[pos++] = (u8)a;

    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    ihdr[0] = (u8)(w >> 24); ihdr[1] = (u8)(w >> 16); ihdr[2] = (u8)(w >> 8); ihdr[3] = (u8)w;
    ihdr[4] = (u8)(h >> 24); ihdr[5] = (u8)(h >> 16); ihdr[6] = (u8)(h >> 8); ihdr[7] = (u8)h;
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   // 8-bit RGB
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, pos);
    chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}
