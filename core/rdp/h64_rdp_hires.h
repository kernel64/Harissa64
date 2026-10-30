// Hi-res texture keys ("Rice" checksums, as GLideN64 computes them for
// community texture packs: NAME#TEXCRC#FMT#SIZE[#PALCRC]_*.png).
// Ported from GLideN64 (GPL v2): src/GLideNHQ/TxUtil.cpp (RiceCRC32,
// checksum64), src/Textures.cpp (_loadHiresTexture), src/gDP.cpp (load info).
#ifndef H64_RDP_HIRES_H
#define H64_RDP_HIRES_H

#include "../common/h64_types.h"

struct H64System;

// A texture as a pack names it.
struct H64HiresKey
{
    u32 texCrc, palCrc;   // palCrc 0: no palette
    u8 fmt, size;         // N64 format (0 RGBA, 1 YUV, 2 CI, 3 IA, 4 I) and size (0 4-bit .. 3 32-bit)
    u8 hasPal;
};

// Starts (on) or stops (off) tracking texture loads and uses.
void h64_rdp_hires_enable(H64System *sys, int on);
// Called by the software RDP: a LOAD_TILE (mode 0), LOAD_TLUT (1) or LOAD_BLOCK (2)
// before it runs, and each tile a drawn primitive samples.
void h64_rdp_hires_on_load(H64System *sys, u32 tileIndex, u32 sl, u32 tl, u32 sh, u32 th, int mode);
void h64_rdp_hires_on_use(H64System *sys, u32 tileIndex);
// The distinct keys seen so far.
u32 h64_rdp_hires_count(H64System *sys);
const H64HiresKey *h64_rdp_hires_key(H64System *sys, u32 i);
// The key of one tile now (0 when it has no recorded load).
int h64_rdp_hires_compute(H64System *sys, u32 tileIndex, H64HiresKey *out);
// The Rice checksum of an area of N64-order memory (width texels, height rows, size 0-3).
u32 h64_rice_crc32(const u8 *src, int width, int height, int size, int rowStride);

#endif
