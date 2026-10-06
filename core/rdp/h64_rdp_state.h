// Harissa64 V2 - software RDP: renderer state.
//
// The software renderer is a serial CPU port of ParaLLEl-RDP's compute
// shaders (MIT licence, Themaister/parallel-rdp at commit 1cecd042, see
// THIRD_PARTY.md), which reproduce the RDP bit-exactly: span setup, coverage,
// attribute interpolation, perspective divide, texture sampling, combiner,
// blender, depth test, dither and framebuffer formats. TMEM loads are written
// here as the hardware does them (walking the load), not as the shaders' GPU
// "gather" formulation.
//
// GLSL integers wrap; the core is built with wrapping signed arithmetic
// (-fwrapv on gcc; MSVC wraps) and these files rely on it.
#ifndef H64_RDP_STATE_H
#define H64_RDP_STATE_H

#include "../common/h64_types.h"

struct H64System;

// Other-modes flags (same bit layout as ParaLLEl-RDP's StaticRasterizationState)
enum
{
    RS_INTERLACE_FIELD = 1 << 0,
    RS_INTERLACE_KEEP_ODD = 1 << 1,
    RS_AA = 1 << 2,
    RS_PERSPECTIVE = 1 << 3,
    RS_TLUT = 1 << 4,
    RS_TLUT_TYPE = 1 << 5,
    RS_CVG_TIMES_ALPHA = 1 << 6,
    RS_ALPHA_CVG_SELECT = 1 << 7,
    RS_MULTI_CYCLE = 1 << 8,
    RS_TEX_LOD = 1 << 9,
    RS_SHARPEN_LOD = 1 << 10,
    RS_DETAIL_LOD = 1 << 11,
    RS_FILL = 1 << 12,
    RS_COPY = 1 << 13,
    RS_SAMPLE_QUAD = 1 << 14,
    RS_ALPHA_TEST = 1 << 15,
    RS_ALPHA_TEST_DITHER = 1 << 16,
    RS_MID_TEXEL = 1 << 17,
    RS_CONVERT_ONE = 1 << 22,
    RS_BILERP0 = 1 << 23,
    RS_BILERP1 = 1 << 24
};

enum
{
    DB_DEPTH_TEST = 1 << 0,
    DB_DEPTH_UPDATE = 1 << 1,
    DB_FORCE_BLEND = 1 << 3,
    DB_IMAGE_READ = 1 << 4,
    DB_COLOR_ON_CVG = 1 << 5,
    DB_MULTI_CYCLE = 1 << 6,
    DB_AA = 1 << 7,
    DB_DITHER = 1 << 8
};

enum { TILE_CLAMP_S = 1, TILE_MIRROR_S = 2, TILE_CLAMP_T = 4, TILE_MIRROR_T = 8 };
enum { FB_I4 = 0, FB_I8, FB_RGBA5551, FB_IA88, FB_RGBA8888 };

struct H64RdpTile
{
    u32 slo, shi, tlo, thi;      // 10.2
    u32 offset, stride;          // bytes
    u8 fmt, size, palette;
    u8 maskS, shiftS, maskT, shiftT;
    u8 flags;
};

struct H64RdpCombiner { u8 rgbMulAdd, rgbMulSub, rgbMul, rgbAdd, aMulAdd, aMulSub, aMul, aAdd; };

struct H64RdpState
{
    u8 tmem[4096];               // N64 byte order (logical)
    H64RdpTile tiles[8];

    u32 rasterFlags;             // RS_*
    u32 dither;                  // other modes bits 4..7
    u32 depthBlendFlags;         // DB_*
    u8 coverageMode, zMode;
    u8 blend[2][4];              // per cycle: 1a, 1b, 2a, 2b
    int usePrimDepth;
    H64RdpCombiner combiner[2];

    u32 primColor, envColor, fogColor, blendColor, fillColor;
    u8 primMinLevel, primLodFrac;
    s32 primDepth;               // z << 16
    u32 primDz;
    s32 convert[6];
    u32 keyWidth[3], keyCenter[3], keyScale[3];

    u32 scissorXlo, scissorYlo, scissorXhi, scissorYhi;

    u32 texAddr, texWidth;
    u8 texFmt, texSize;
    u32 colorAddr, colorWidth;
    int colorFmt;                // FB_*
    u32 depthAddr;

    u16 noise;                   // current noise sample
    u32 primitives;              // statistics
};

// Texture unit (h64_rdp_tex.cpp)
struct H64RdpTexel { s32 c[4]; };   // r, g, b, a (or u, v, y, y for YUV)
void h64_rdp_load(H64System *sys, u32 tile, u32 sl, u32 tl, u32 sh, u32 th, int mode);   // 0 tile, 1 TLUT, 2 block
void h64_rdp_sample(H64RdpState *st, const H64RdpTile *tile, const s32 *stIn, int tlut, int tlutType, int sampleQuad,
                    int midTexel, int convertOne, int bilerp, const s32 *factors, const H64RdpTexel *prev, H64RdpTexel *out);
s32 h64_rdp_sample_copy(H64RdpState *st, const H64RdpTile *tile, s32 s, s32 t, int sOffset, int tlut, int fbSize);
void h64_rdp_texture_convert(const H64RdpTexel *in, const s32 *factors, H64RdpTexel *out);
void h64_rdp_compute_lod(u32 *tile0, u32 *tile1, s32 *lodFrac, u32 maxLevel, s32 minLod, const s32 *st, const s32 *stDx,
                         const s32 *stDy, int perspectiveOverflow, int texLod, int sharpen, int detail);

// Helpers shared by the RDP files
static inline s32 h64_sext(s32 v, int bits) { return (s32)((u32)v << (32 - bits)) >> (32 - bits); }
static inline s32 h64_clamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int h64_find_msb(u32 v) { int n = -1; while (v) { v >>= 1; n++; } return n; }
static inline int h64_find_lsb(u32 v) { int n = 0; if (!v) return -1; while (!(v & 1)) { v >>= 1; n++; } return n; }

#endif
