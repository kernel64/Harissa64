// Hi-res texture keys: the "Rice" checksums community texture packs are named
// by. Ported from GLideN64 (GPL v2, https://github.com/gonetz/GLideN64):
// TxUtil::RiceCRC32 / RiceCRC32_CI4 / RiceCRC32_CI8 / checksum64 and
// CalculateMaxCI4b/8b (src/GLideNHQ/TxUtil.cpp), the size and row stride
// rules of TextureCache::_loadHiresTexture and ReverseDXT (src/Textures.cpp),
// and the load bookkeeping of gDPLoadTile / gDPLoadBlock / gDPLoadTLUT
// (src/gDP.cpp). GLideN64 keeps RDRAM as host-order 32-bit words; here memory
// is in N64 order, so its 32-bit reads are big-endian loads.
#include "h64_rdp_hires.h"
#include "h64_rdp_state.h"
#include "../common/h64_endian.h"
#include "../system/h64_system.h"

#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {

const u32 PAL_BASE = 512, PAL_MASK = 1023;

struct LoadInfo   // gDPLoadTileInfo
{
    u32 texAddress, uls, ult, lrs, lrt, width, height, texWidth, dxt;
    u8 size, loadType, valid;   // loadType 0 tile, 1 block
};

struct Track
{
    LoadInfo info[512];   // by TMEM address (64-bit words)
    // GLideN64's TexFilterPalette (256 entries, raw memory order) at PAL_BASE: its
    // checksum of a one-colour palette reads the 2 bytes before it.
    u8 palette[1024];
    int mk64;             // Mario Kart 64: GLideN64's hack_MK64
    std::vector<H64HiresKey> keys;
};

Track *track(H64System *sys)
{
    return sys->rdpState ? (Track *)sys->rdpState->hires : 0;
}

const u8 *mem(H64System *sys)
{
    const H64RdpState *st = sys->rdpState;
    return st->loadRam ? st->loadRam : sys->rdram;
}

// A 32-bit read as GLideN64 does it: its RDRAM holds host-order (little-endian)
// words, so byte a of its memory is N64 byte a ^ 3, also for reads that are not
// 4-byte aligned (a palette of an odd number of entries).
u32 rd32(const u8 *m, u32 a, u32 k)
{
    return (u32)m[(a ^ 3) & k] | ((u32)m[((a + 1) ^ 3) & k] << 8) | ((u32)m[((a + 2) ^ 3) & k] << 16) |
           ((u32)m[((a + 3) ^ 3) & k] << 24);
}

// (bytesPerLine below 4: GLideN64's loop still runs once, at a negative offset.)
u32 rice_crc(const u8 *m, u32 src, int width, int height, int size, int rowStride, u32 mask = H64_RDRAM_SIZE - 1)
{
    u32 crc = 0;
    const int bytesPerLine = (width << size) >> 1;
    int y = height - 1;
    if (bytesPerLine <= 0 || height <= 0) return 0;
    do
    {
        u32 esi = 0;
        int x = bytesPerLine - 4;
        do
        {
            esi = rd32(m, src + (u32)x, mask) ^ (u32)x;
            crc = (crc << 4) + ((crc >> 28) & 15);
            crc += esi;
            x -= 4;
        } while (x >= 0);
        esi ^= (u32)y;
        crc += esi;
        src += (u32)rowStride;
        --y;
    } while (y >= 0);
    return crc;
}

// Largest colour index (GLideN64 reads its word-swapped memory byte by byte:
// the same bytes, in another order within each word; the maximum is the same).
u32 max_ci(const u8 *m, u32 src, int width, int height, int rowStride, int four)
{
    u32 v = 0;
    int bytes = four ? width >> 1 : width, x, y;
    for (y = 0; y < height; y++)
        for (x = 0; x < bytes; x++)
        {
            u8 b = m[(src + (u32)(y * rowStride + x)) & (H64_RDRAM_SIZE - 1)];
            if (four) { if ((u32)(b >> 4) > v) v = b >> 4; if ((u32)(b & 15) > v) v = b & 15; }
            else if (b > v) v = b;
        }
    return v;
}

u32 calc_dxt(u32 t) { return t ? (2048 + t - 1) / t : 1; }
u32 txl2words(u32 width, u32 size)
{
    static const u32 bytes[4] = { 0, 1, 2, 4 };
    if (size == 0) return width / 16 > 1 ? width / 16 : 1;
    return width * bytes[size] / 8 > 1 ? width * bytes[size] / 8 : 1;
}
u32 reverse_dxt(u32 val, u32 width, u32 size)
{
    int low, high, i;
    if (val == 0x800) return 1;
    low = 2047 / (int)val;
    if (calc_dxt((u32)low) > val) low++;
    high = 2047 / ((int)val - 1);
    if (low == high) return (u32)low;
    for (i = low; i <= high; i++)
        if (txl2words(width, size) == (u32)i) return (u32)i;
    return (u32)((low + high) / 2);
}

}   // namespace

u32 h64_rice_crc32(const u8 *src, int width, int height, int size, int rowStride)
{
    // An address-0 view of `src` (rd32 masks with the RDRAM size: callers pass RDRAM-sized data).
    return rice_crc(src, 0, width, height, size, rowStride);
}

void h64_rdp_hires_enable(H64System *sys, int on)
{
    H64RdpState *st = h64_rdp_state(sys);
    if (on && !st->hires)
    {
        Track *t = new Track();
        memset(t->info, 0, sizeof(t->info));
        memset(t->palette, 0, sizeof(t->palette));
        t->mk64 = !memcmp(sys->rom.gameCode, "NKT", 3);
        st->hires = t;
    }
    if (!on && st->hires) { delete (Track *)st->hires; st->hires = 0; }
}

int h64_rdp_hires_enabled(H64System *sys)
{
    return track(sys) != 0;
}

void h64_rdp_hires_on_load(H64System *sys, u32 tileIndex, u32 sl, u32 tl, u32 sh, u32 th, int mode)
{
    Track *t = track(sys);
    const H64RdpState *st = sys->rdpState;
    const H64RdpTile *tile;
    u32 tmem;
    if (!t) return;
    tile = &st->tiles[tileIndex & 7];
    tmem = (tile->offset >> 3) & 511;
    if (mode == 1)
    {
        // gDPLoadTLUT: the entries copied raw from the texture image's start.
        u32 count = (((sh >> 2) - (sl >> 2) + 1) * ((th >> 2) - (tl >> 2) + 1)) & 0xFFFF, start, i;
        if (tmem < 256) return;
        start = tmem - 256;
        for (i = 0; i < count * 2 && start * 2 + i < 512; i++)
            t->palette[PAL_BASE + start * 2 + i] = mem(sys)[(st->texAddr + i) & (H64_RDRAM_SIZE - 1)];
        return;
    }
    {
        LoadInfo &li = t->info[tmem];
        li.texAddress = st->texAddr & 0xFFFFFF;
        li.uls = sl >> 2; li.ult = tl >> 2; li.lrs = sh >> 2; li.lrt = th >> 2;
        li.size = st->texSize;
        li.texWidth = st->texWidth;
        li.valid = 1;
        if (mode == 0)
        {
            u32 width = li.lrs - li.uls + 1, height = li.lrt - li.ult + 1;
            li.width = tile->maskS ? (width < (1u << tile->maskS) ? width : (1u << tile->maskS)) : width;
            li.height = tile->maskT ? (height < (1u << tile->maskT) ? height : (1u << tile->maskT)) : height;
            li.loadType = 0;
            li.dxt = 0;
        }
        else
        {
            // gDPLoadBlock: sh is the last texel (lrs), th the dxt.
            u32 lo = tmem, hi, k;
            li.width = sh;      // info.width = lrs (raw)
            li.dxt = th;
            li.loadType = 1;
            hi = tmem + ((((((sh - sl + 1) & 0xFFF) << st->texSize) >> 1) + 7) >> 3);
            // Tiles whose TMEM lies in the block are now block loads too.
            for (k = 0; k < 8; k++)
            {
                u32 tm = (st->tiles[k].offset >> 3) & 511;
                if (k != (tileIndex & 7) && tm >= lo && tm < hi) t->info[tm].loadType = 1;
            }
        }
    }
}

int h64_rdp_hires_compute(H64System *sys, u32 tileIndex, H64HiresKey *out)
{
    Track *t = track(sys);
    const H64RdpState *st = sys->rdpState;
    const H64RdpTile *tile;
    const LoadInfo *li;
    const u8 *m;
    u32 addr, bpl, width, height, tw, th;
    if (!t) return 0;
    tile = &st->tiles[tileIndex & 7];
    li = &t->info[(tile->offset >> 3) & 511];
    if (!li->valid || !li->texAddress) return 0;
    m = mem(sys);
    // The tile's own size (gSP.textureTile, CachedTexture width).
    tw = (((tile->shi >> 2) - (tile->slo >> 2)) & 0x3FF) + 1;
    th = (((tile->thi >> 2) - (tile->tlo >> 2)) & 0x3FF) + 1;
    addr = li->texAddress;
    if (li->loadType == 0)
    {
        bpl = (li->texWidth << li->size) >> 1;
        addr += li->ult * bpl + (((li->uls << li->size) + 1) >> 1);
        width = li->width < li->texWidth ? li->width : li->texWidth;
        if (li->size > tile->size) width <<= li->size - tile->size;
        height = li->height;
        // GLideN64's hack_MK64 (Mario Kart 64): odd heights lose their last row.
        if (t->mk64 && (height & 1)) height--;
    }
    else
    {
        u32 maskW = tile->maskS ? 1u << tile->maskS : tw, maskH = tile->maskT ? 1u << tile->maskT : th;
        int clampS = (tile->flags & TILE_CLAMP_S) != 0, clampT = (tile->flags & TILE_CLAMP_T) != 0;
        width = (clampS && tw <= 256) ? (maskW < tw ? maskW : tw) : maskW;
        height = ((clampT && th <= 256) || maskH > 256) ? (maskH < th ? maskH : th) : maskH;
        if (tile->size == 3) bpl = (tile->stride >> 3) << 4;     // line << 4
        else if (li->dxt == 0) bpl = tile->stride;               // line << 3 (stride is line * 8)
        else
        {
            u32 dxt = li->dxt;
            if (dxt > 1) dxt = reverse_dxt(dxt, tw, tile->size);
            bpl = dxt << 3;
        }
    }
    memset(out, 0, sizeof(*out));
    out->width = (u16)width;
    out->height = (u16)height;
    out->fmt = tile->fmt;
    out->size = tile->size;
    if (tile->size < 2 && ((st->rasterFlags & RS_TLUT) || tile->fmt == 2))
    {
        // checksum64 with a palette: CI texture CRC and the palette's CRC up to the largest index.
        if (tile->size == 1)
        {
            u32 cimax = max_ci(m, addr, (int)width, (int)height, (int)bpl, 0);
            out->texCrc = rice_crc(m, addr, (int)width, (int)height, 1, (int)bpl);
            out->palCrc = rice_crc(t->palette, PAL_BASE, (int)cimax + 1, 1, 2, 512, PAL_MASK);
        }
        else
        {
            u32 cimax = max_ci(m, addr, (int)width, (int)height, (int)bpl, 1);
            out->texCrc = rice_crc(m, addr, (int)width, (int)height, 0, (int)bpl);
            out->palCrc = rice_crc(t->palette, PAL_BASE + tile->palette * 32u, (int)cimax + 1, 1, 2, 32, PAL_MASK);
        }
        out->hasPal = 1;
        if (out->palCrc || out->texCrc) return 1;
    }
    out->hasPal = 0;
    out->palCrc = 0;
    out->texCrc = rice_crc(m, addr, (int)width, (int)height, (int)tile->size, (int)bpl);
    return 1;
}

void h64_rdp_hires_on_use(H64System *sys, u32 tileIndex)
{
    Track *t = track(sys);
    H64HiresKey k;
    size_t i;
    if (!t || !h64_rdp_hires_compute(sys, tileIndex, &k)) return;
    for (i = 0; i < t->keys.size(); i++)
    {
        const H64HiresKey &e = t->keys[i];
        if (e.texCrc == k.texCrc && e.palCrc == k.palCrc && e.fmt == k.fmt && e.size == k.size && e.hasPal == k.hasPal) return;
    }
    t->keys.push_back(k);
}

u32 h64_rdp_hires_count(H64System *sys)
{
    Track *t = track(sys);
    return t ? (u32)t->keys.size() : 0;
}

const H64HiresKey *h64_rdp_hires_key(H64System *sys, u32 i)
{
    Track *t = track(sys);
    return t && i < t->keys.size() ? &t->keys[i] : 0;
}
