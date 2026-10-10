// Harissa64 V2 - graphics task HLE (see h64_gfx.h).
//
// Microcode behaviour (command encodings, matrix stack, lights, viewport,
// texture generation, fog, branch and cull commands) follows GLideN64's
// gSP.cpp, GBI.cpp and uCodes/F3D*.cpp (GPL v2, commit 41c7ba2, August 2026),
// rewritten for V2: no renderer state is kept here, triangles leave as
// screen-space vertices (render/api.h) after clipping and culling.
#include "h64_gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

#if defined(_XBOX)
#include <ppcintrinsics.h>
#endif
#include "../common/h64_crc32.h"
#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../common/h64_mem.h"
#include "../rdp/h64_rdp.h"
#include "../rdp/h64_rdp_state.h"
#include "../system/h64_system.h"
#include "../../render/api.h"

enum { UC_NONE = 0, UC_F3D, UC_F3DEX, UC_F3DEX2, UC_S2DEX2 };
// Rare's modified microcodes, recognised by the CRC of their code (GLideN64's
// table): F3D with other commands and vertex formats.
enum { RARE_NONE = 0, RARE_GOLDEN, RARE_PD, RARE_DKR, RARE_JFG, RARE_CBFD };

// Geometry mode bits, as decoded (the microcodes place them differently).
enum
{
    GM_ZBUFFER = 1,
    GM_SHADE = 2,
    GM_SMOOTH = 4,
    GM_CULL_FRONT = 8,
    GM_CULL_BACK = 16,
    GM_FOG = 32,
    GM_LIGHTING = 64,
    GM_TEXGEN = 128,
    GM_TEXGEN_LINEAR = 256
};

// Vertex clip flags.
enum { CL_POSX = 1, CL_NEGX = 2, CL_POSY = 4, CL_NEGY = 8, CL_W = 16 };

#define GFX_VTX_MAX 80
#define GFX_STACK_MAX 32
#define GFX_PC_MAX 18
#define GFX_MAX_COMMANDS 4000000u

struct GfxUcode
{
    u32 start, dstart, dsize;
    int type;
    int zex;     // F3DZEX (Zelda): G_BRANCH_W instead of G_BRANCH_Z
    int noNear;  // ".NoN": no near-plane clipping
    int rare;    // RARE_*
};

struct GfxVtx
{
    float x, y, z, w;        // clip space
    float r, g, b, a;        // 0..255
    float s, t;              // s10.5 units, texture scale applied
    float nx, ny, nz;        // normal (lighting, texture generation)
    u32 clip;
    int xyOverride;          // G_MWO_POINT_XYSCREEN: screen position given
    float sx, sy;
    int zOverride;           // G_MWO_POINT_ZSCREEN
    float sz;
};

struct GfxOp
{
    u32 kind;    // 0: RDP command (words[index..index+count)), 1: triangle tris[index]
    u32 index, count;
};

struct GfxTri
{
    H64RenderVertex v[3];
    u32 flags, tile, levels;
};

struct H64Gfx
{
    H64System *sys;
    GfxUcode ucodes[16];
    int ucodeCount, ucodeNext;
    const GfxUcode *uc;

    u32 segments[16];
    float mv[GFX_STACK_MAX][4][4];
    int mvi, stackSize;
    float proj[4][4], combined[4][4];
    int combinedValid, forcedCombined;
    GfxVtx vtx[GFX_VTX_MAX];
    u32 geomRaw;
    u32 omH, omL;
    float lightCol[10][3];
    float lightDir[10][3];
    int numLights;
    float lookat[2][3];
    float vscale[4], vtrans[4];
    float clipRatio;
    float texScaleS, texScaleT;
    u32 texLevel, texTile, texOn;
    float fogMul, fogOff;
    u32 half1;
    // Rare microcodes: DMA offsets, billboards, vertex colour table, counted display lists
    u32 dmaMtx, dmaVtx, colorBase;
    u32 texOffset, texShift, texCount;
    u32 timgW0, timgW1;
    int billboard, vertexi, dlCount;
    // Conker's Bad Fur Day: point lights, a separate normal table, coordinate modifiers
    float lightPos[10][3], lightCa[10];
    float coordMod[16];
    u32 normalBase;
    int advLighting;
    u32 pc[GFX_PC_MAX];
    int pci, halt, abort;
    int fullSync;
    u32 commands;
    u32 nVerts, nTris;      // this task's vertices and triangles (cost estimate)
    u32 warned;

    std::vector<u64> words;
    std::vector<GfxTri> tris;
    std::vector<GfxOp> ops;

    // Deferred rendering: what texture loads read (snapshot), colour images seen.
    u32 timgAddr, timgWidth, timgSize;
    std::vector<u32> loadRanges;   // start, end pairs
    std::vector<u64> rangeSort;    // the same as start << 32 | end, sorted (h64_gfx_parse_task)
    u32 cimg[8][2];                // recent colour images: start, end
    u32 cimgNext;
    int texFromCimg;
    u8 *snapshot;                  // RDRAM-sized; only the ranges above are valid
};

// ---------------------------------------------------------------- memory
static u32 seg_to_phys(H64Gfx *g, u32 a)
{
    return (g->segments[(a >> 24) & 0x0F] + (a & 0x00FFFFFF)) & 0x00FFFFFF;
}

static u32 rd32(H64Gfx *g, u32 a)
{
    a &= 0x7FFFFC;
    return h64_load_be32(g->sys->rdram + a);
}

static s16 rd16(H64Gfx *g, u32 a) { return (s16)h64_load_be16(g->sys->rdram + (a & 0x7FFFFE)); }
static u8 rd8(H64Gfx *g, u32 a) { return g->sys->rdram[a & 0x7FFFFF]; }

// Integers to floats through tables: on the Xbox 360's CPU each conversion
// goes through memory (a load-hit-store stall of ~40 cycles), a dozen per
// vertex (Conker's cutscenes: ~11 ms a frame loading vertices). Exact.
static float s_u8f[256], s_s8f[256], s_s8n[256];   // b, (s8)b, (s8)b / 128
static void init_tables(void)
{
    int i;
    for (i = 0; i < 256; i++)
    {
        s_u8f[i] = (float)i;
        s_s8f[i] = (float)(s8)i;
        s_s8n[i] = (float)(s8)i / 128.0f;
    }
}
static float rdf16(H64Gfx *g, u32 a)
{
    const u8 *p = g->sys->rdram + (a & 0x7FFFFE);
    return s_s8f[p[0]] * 256.0f + s_u8f[p[1]];
}
static float rdf8(H64Gfx *g, u32 a) { return s_u8f[g->sys->rdram[a & 0x7FFFFF]]; }
static float rdn8(H64Gfx *g, u32 a) { return s_s8n[g->sys->rdram[a & 0x7FFFFF]]; }

static void load_matrix(H64Gfx *g, u32 phys, float m[4][4])
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
        {
            u32 k = (u32)(i * 4 + j) * 2;
            s32 v = (s32)(((u32)(u16)rd16(g, phys + k) << 16) | (u16)rd16(g, phys + 32 + k));
            m[i][j] = (float)v / 65536.0f;
        }
}

static void mat_mul(float out[4][4], const float a[4][4], const float b[4][4])
{
    float r[4][4];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, r, sizeof(r));
}

static void mat_identity(float m[4][4])
{
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

// ---------------------------------------------------------------- output
static void out_rdp(H64Gfx *g, const u64 *w, u32 n)
{
    GfxOp op;
    u32 i;
    op.kind = 0;
    op.index = (u32)g->words.size();
    op.count = n;
    for (i = 0; i < n; i++) g->words.push_back(w[i]);
    g->ops.push_back(op);
}

// Texture images and loads (h64_rdp_load's arithmetic): the RDRAM a load reads.
static void note_rdp(H64Gfx *g, u32 w0, u32 w1)
{
    u32 op = (w0 >> 24) & 0x3F;
    if (op == 0x3D)   // SET_TEXTURE_IMAGE
    {
        u32 k;
        g->timgSize = (w0 >> 19) & 3;
        g->timgWidth = (w0 & 0x3FF) + 1;
        g->timgAddr = w1 & 0xFFFFFF;
        for (k = 0; k < 8; k++)
            if (g->timgAddr >= g->cimg[k][0] && g->timgAddr < g->cimg[k][1]) g->texFromCimg = 1;
    }
    else if (op == 0x3F)   // SET_COLOR_IMAGE: remember it (a later texture may read it)
    {
        u32 size = (w0 >> 19) & 3, width = (w0 & 0x3FF) + 1, start = w1 & 0xFFFFFF;
        u32 bytes = width * (width <= 320 ? 240 : width * 3 / 4) << (size ? size - 1 : 0);
        g->cimg[g->cimgNext & 7][0] = start;
        g->cimg[g->cimgNext & 7][1] = start + bytes;
        g->cimgNext++;
    }
    else if (op == 0x30 || op == 0x33 || op == 0x34)   // LOAD_TLUT, LOAD_BLOCK, LOAD_TILE
    {
        u32 sl = (w0 >> 12) & 0xFFF, tl = w0 & 0xFFF, sh = (w1 >> 12) & 0xFFF, th = w1 & 0xFFF;
        u32 sz = g->timgSize ? g->timgSize : 1, start, end;
        if (op == 0x33)
        {
            u32 texels = (sh - sl + 1) & 0xFFF;
            start = g->timgAddr + ((tl * g->timgWidth + sl) << (sz - 1));
            end = start + ((((texels << sz) + 15) >> 4) * 8);
        }
        else
        {
            u32 lines = (th >> 2) >= (tl >> 2) ? (th >> 2) - (tl >> 2) + 1 : 1;
            u32 pixels = ((sh >> 2) - (sl >> 2) + 1) & 0xFFF;
            start = g->timgAddr + (((tl >> 2) * g->timgWidth + (sl >> 2)) << (sz - 1));
            end = start + (lines - 1) * (g->timgWidth << (sz - 1)) + ((((pixels << sz) + 15) >> 4) * 8);
            if (op == 0x30) end = start + pixels * 2 + 16;   // TLUT: one line of 16-bit entries
        }
        start &= ~7u;
        end = (end + 15) & ~7u;
        if (start < H64_RDRAM_SIZE)
        {
            if (end > H64_RDRAM_SIZE || end < start) end = H64_RDRAM_SIZE;
            g->loadRanges.push_back(start);
            g->loadRanges.push_back(end);
        }
    }
}

static void out_rdp1(H64Gfx *g, u32 w0, u32 w1)
{
    note_rdp(g, w0, w1);
    u64 w = ((u64)w0 << 32) | w1;
    out_rdp(g, &w, 1);
}

static void out_othermode(H64Gfx *g)
{
    out_rdp1(g, 0xEF000000u | (g->omH & 0x00FFFFFF), g->omL);
}

static void out_tri(H64Gfx *g, const H64RenderVertex *a, const H64RenderVertex *b, const H64RenderVertex *c, u32 flags)
{
    GfxTri t;
    GfxOp op;
    t.v[0] = *a;
    t.v[1] = *b;
    t.v[2] = *c;
    t.flags = flags;
    t.tile = g->texTile;
    t.levels = g->texLevel + 1;
    op.kind = 1;
    op.index = (u32)g->tris.size();
    op.count = 0;
    g->tris.push_back(t);
    g->ops.push_back(op);
}

// ---------------------------------------------------------------- microcode detection
static void detect_ucode(H64Gfx *g, GfxUcode *u)
{
    char data[2048 + 1];
    u32 i, n = u->dsize ? u->dsize : 2048;
    u->type = UC_NONE;
    u->zex = 0;
    u->noNear = 0;
    u->rare = RARE_NONE;
    {
        // CRC-32 of the first 4 KB of code as GLideN64 computes it (its
        // RDRAM holds 32-bit words byte-reversed).
        static const struct { u32 crc; int type, rare, noNear; const char *name; } known[] = {
            { 0x302BCA09u, UC_F3D, RARE_GOLDEN, 0, "GoldenEye" },
            { 0x1C4F7869u, UC_F3D, RARE_PD, 0, "Perfect Dark" },
            { 0x6E6FC893u, UC_F3D, RARE_DKR, 0, "Diddy Kong Racing" },
            { 0x8D91244Fu, UC_F3D, RARE_DKR, 0, "Diddy Kong Racing" },
            { 0xBDE9D1FBu, UC_F3D, RARE_JFG, 0, "Jet Force Gemini, Mickey's Speedway" },
            { 0x1B4ACE88u, UC_F3DEX2, RARE_CBFD, 1, "Conker's Bad Fur Day" },   // F3DEXBG.NoN
        };
        u8 code[4096];
        u32 crc, k;
        for (k = 0; k < 4096; k += 4)
        {
            u32 w = rd32(g, u->start + k);
            code[k] = (u8)w;
            code[k + 1] = (u8)(w >> 8);
            code[k + 2] = (u8)(w >> 16);
            code[k + 3] = (u8)(w >> 24);
        }
        crc = h64_crc32(0, code, sizeof(code));
        for (k = 0; k < sizeof(known) / sizeof(known[0]); k++)
            if (known[k].crc == crc)
            {
                u->type = known[k].type;
                u->rare = known[k].rare;
                u->noNear = known[k].noNear;
                H64_INFO("[gfx] microcode at %06X: CRC %08X, %s's variant", u->start, crc, known[k].name);
                return;
            }
    }
    if (n > 2048) n = 2048;
    for (i = 0; i < n; i++) data[i] = (char)rd8(g, u->dstart + i);
    data[n] = 0;
    for (i = 0; i + 3 < n; i++)
    {
        char str[256];
        u32 j = 0;
        if (data[i] != 'R' || data[i + 1] != 'S' || data[i + 2] != 'P') continue;
        memset(str, 0, sizeof(str));
        while (i + j < n && j < 255 && data[i + j] > 0x0A) { str[j] = data[i + j]; j++; }
        str[j] = 0;
        if (!strncmp(str + 4, "SW", 2))
            u->type = UC_F3D;
        else if (!strncmp(str + 4, "Gfx", 3) && j > 31 && !strncmp(str + 14, "F3D", 3))
        {
            u->noNear = strstr(str + 4, ".NoN") != 0 || strstr(str + 4, ".Rej") != 0;
            if (!strncmp(str + 14, "F3DZEX", 6))
            {
                u->type = UC_F3DEX2;
                u->zex = 1;
            }
            else if (!strncmp(str + 14, "F3DTEX/A", 8) || !strncmp(str + 14, "F3DAM", 5) ||
                     !strncmp(str + 14, "F3DFLX", 6) || !strncmp(str + 14, "F3DLP", 5))
                u->type = UC_NONE;   // variants not handled yet
            else if (str[28] == '1' || !strncmp(str + 28, "0.95", 4) || !strncmp(str + 28, "0.96", 4))
                u->type = UC_F3DEX;
            else if (j > 31 && str[31] == '2')
                u->type = UC_F3DEX2;
        }
        else if (!strncmp(str + 4, "Gfx", 3) && j > 19 && !strncmp(str + 14, "S2DEX", 5))
        {
            // S2DEX2 (the F3DEX2-era 2D microcode, OoT's backgrounds): version 2.xx.
            const char *v = strstr(str, "fifo ");
            if (!v) v = strstr(str, "xbus ");
            if (v && v[5] == '2') u->type = UC_S2DEX2;
        }
        H64_INFO("[gfx] microcode \"%s\" at %06X: %s", str, u->start,
                 u->type == UC_F3D ? "F3D" : u->type == UC_F3DEX ? "F3DEX" :
                 u->type == UC_F3DEX2 ? (u->zex ? "F3DZEX" : "F3DEX2") : u->type == UC_S2DEX2 ? "S2DEX2" : "not handled (LLE)");
        return;
    }
    H64_INFO("[gfx] microcode at %06X: no name string, not handled (LLE)", u->start);
}

static const GfxUcode *find_ucode(H64Gfx *g, u32 start, u32 dstart, u32 dsize)
{
    int i;
    GfxUcode *u;
    for (i = 0; i < g->ucodeCount; i++)
        if (g->ucodes[i].start == start && g->ucodes[i].dstart == dstart && g->ucodes[i].dsize == dsize)
            return &g->ucodes[i];
    u = &g->ucodes[g->ucodeNext];
    g->ucodeNext = (g->ucodeNext + 1) & 15;
    if (g->ucodeCount < 16) g->ucodeCount++;
    u->start = start;
    u->dstart = dstart;
    u->dsize = dsize;
    detect_ucode(g, u);
    return u;
}

// ---------------------------------------------------------------- geometry mode
static u32 geom(const H64Gfx *g)
{
    u32 r = g->geomRaw, m = 0;
    if (r & 0x1) m |= GM_ZBUFFER;
    if (r & 0x4) m |= GM_SHADE;
    if (r & 0x10000) m |= GM_FOG;
    if (r & 0x20000) m |= GM_LIGHTING;
    if (r & 0x40000) m |= GM_TEXGEN;
    if (r & 0x80000) m |= GM_TEXGEN_LINEAR;
    if (g->uc->type == UC_F3DEX2)
    {
        if (r & 0x200) m |= GM_CULL_FRONT;
        if (r & 0x400) m |= GM_CULL_BACK;
        if (r & 0x200000) m |= GM_SMOOTH;
    }
    else
    {
        if (r & 0x200) m |= GM_SMOOTH;
        if (r & 0x1000) m |= GM_CULL_FRONT;
        if (r & 0x2000) m |= GM_CULL_BACK;
    }
    return m;
}

// ---------------------------------------------------------------- vertices
static void update_combined(H64Gfx *g)
{
    if (!g->combinedValid)
    {
        mat_mul(g->combined, g->mv[g->mvi], g->proj);
        g->combinedValid = 1;
    }
}

// Direction `d` from eye space into model space (transpose of the model-view
// rotation), normalised.
static void to_model_dir(const float m[4][4], const float d[3], float out[3])
{
    float x = m[0][0] * d[0] + m[0][1] * d[1] + m[0][2] * d[2];
    float y = m[1][0] * d[0] + m[1][1] * d[1] + m[1][2] * d[2];
    float z = m[2][0] * d[0] + m[2][1] * d[1] + m[2][2] * d[2];
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0) { x /= len; y /= len; z /= len; }
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

// Vertex formats: the standard 16-byte Vtx; Perfect Dark's 12-byte vertex with
// a colour/normal index into a table (G_VTXCOLORBASE); Diddy Kong Racing's
// 10-byte vertex (position and colour, no texture coordinates or normal).
enum { VF_STD = 0, VF_PD, VF_DKR, VF_CBFD };

// Conker's lighting (GLideN64 gSPLightVertexCBFD_basic/_advanced): the
// vertex colour times ambient + point lights (distance falloff from the
// light's position, given in clip space after the coordinate modifiers),
// plus in the advanced mode the last light as a directional one. Normals come
// from the G_MV_NORMALES table (x, y) and the vertex's flag (z); a negative
// flag leaves the colour unlit.
// a >= 0 ? b : c without a branch on the Xbox 360 (fsel): a branch on a float
// compare waits for the FPU and often mispredicts (~30 cycles there).
static H64_INLINE float fsel(float a, float b, float c)
{
#if defined(_XBOX)
    return (float)__fsel(a, b, c);
#else
    return a >= 0.0f ? b : c;
#endif
}

// Conker's lighting (after GLideN64): ambient + point lights + in the
// advanced mode a directional one. Returns the factors the vertex colour is
// multiplied by. Scalars in and out, inlined: see load_vertices_body.
static H64_INLINE void cbfd_light(const H64Gfx *g, float x, float y, float z, float nx, float ny, float nz, const float ldir[][3],
                                  const float lc[][3], float *fr, float *fg, float *fb)
{
    int nl = g->numLights > 8 ? 8 : g->numLights, l;
    float r, gg, b, p0, p1, p2;
    const float *cm = g->coordMod;
    r = lc[nl][0];
    gg = lc[nl][1];
    b = lc[nl][2];
    p0 = (x + cm[8]) * cm[12];
    p1 = (y + cm[9]) * cm[13];
    p2 = (z + cm[10]) * cm[14];
    // Branch-free, the same results: min(x, 1) = fsel(x - 1, 1, x); a
    // contribution that is not positive adds an exact zero (colours are >= 0).
    l = nl - 1;
    if (g->advLighting && l >= 0)
    {
        float d = nx * ldir[l][0] + ny * ldir[l][1] + nz * ldir[l][2];
        d = fsel(d - 1.0f, 1.0f, d);
        d = fsel(-d, 0.0f, d);
        r += lc[l][0] * d;
        gg += lc[l][1] * d;
        b += lc[l][2] * d;
    }
    {
        int facing = g->advLighting && (g->geomRaw & 0x00400000u);   // G_POINT_LIGHTING: also facing the light
        for (l = nl - 2; l >= 0; l--)
        {
            float dx = p0 - g->lightPos[l][0], dy = p1 - g->lightPos[l][1], dz = p2 - g->lightPos[l][2];
            float len = (dx * dx + dy * dy + dz * dz) * (1.0f / 32768.0f);   // 2 / 65536: exact
            float in = fsel(-len, 1.0f, g->lightCa[l] / len);                // len > 0 ? ca / len : 1
            in = fsel(in - 1.0f, 1.0f, in);
            if (facing)
            {
                float d = nx * ldir[l][0] + ny * ldir[l][1] + nz * ldir[l][2];
                in *= fsel(d - 1.0f, 1.0f, d);
            }
            in = fsel(-in, 0.0f, in);
            r += lc[l][0] * in;
            gg += lc[l][1] * in;
            b += lc[l][2] * in;
        }
    }
    *fr = r > 1.0f ? 1.0f : r;
    *fg = gg > 1.0f ? 1.0f : gg;
    *fb = b > 1.0f ? 1.0f : b;
}

static void load_vertices_body(H64Gfx *g, u32 phys, u32 n, u32 v0, int fmt);
static void load_vertices_fmt(H64Gfx *g, u32 phys, u32 n, u32 v0, int fmt)
{
    u64 t0 = h64_prof_now(g->sys);
    load_vertices_body(g, phys, n, v0, fmt);
    g->sys->prof[H64_PROF_GFX_VTX] += h64_prof_now(g->sys) - t0;
    g->sys->prof[H64_PROF_GFX_NVTX] += n;
}

// Each vertex is read (bytes) and computed in locals, and written once at the
// end: byte reads may alias anything, so with the results stored as they were
// computed the compiler read each one back from memory right after storing it
// (load-hit-store stalls, ~40 cycles each on the Xbox 360: ~3000 cycles a
// vertex in Conker's cutscenes).
static void load_vertices_body(H64Gfx *g, u32 phys, u32 n, u32 v0, int fmt)
{
    u32 i, stride = fmt == VF_PD ? 12 : fmt == VF_DKR ? 10 : 16;
    u32 gm = geom(g);
    float ldir[10][3], look[2][3], lc[10][3], m[4][4];
    float texS = g->texScaleS, texT = g->texScaleT, fogMul = g->fogMul, fogOff = g->fogOff;
    int l, lighting = (gm & GM_LIGHTING) && fmt != VF_DKR, cbfd = (gm & GM_LIGHTING) && fmt == VF_CBFD;
    int billboard = fmt == VF_DKR && g->billboard, nLights = g->numLights;
    g->nVerts += n;
    if (v0 >= GFX_VTX_MAX) return;
    if (v0 + n > GFX_VTX_MAX) n = GFX_VTX_MAX - v0;
    update_combined(g);
    memcpy(m, g->combined, sizeof(m));
    if (gm & GM_LIGHTING)
        for (l = 0; l < nLights && l < 8; l++) to_model_dir(g->mv[g->mvi], g->lightDir[l], ldir[l]);
    if (cbfd)
        for (l = 0; l < 10; l++)
        {
            lc[l][0] = g->lightCol[l][0] / 255.0f;
            lc[l][1] = g->lightCol[l][1] / 255.0f;
            lc[l][2] = g->lightCol[l][2] / 255.0f;
        }
    if (gm & GM_TEXGEN)
    {
        to_model_dir(g->mv[g->mvi], g->lookat[0], look[0]);
        to_model_dir(g->mv[g->mvi], g->lookat[1], look[1]);
    }
    for (i = 0; i < n; i++)
    {
        GfxVtx *v = &g->vtx[v0 + i];
        u32 a = phys + i * stride, clip = 0;
        float px = rdf16(g, a), py = rdf16(g, a + 2), pz = rdf16(g, a + 4);
        float s = 0, t = 0, x, y, z, w, r, gg, b, al, nx = 0, ny = 0, nz = 0;
        u32 c = fmt == VF_PD ? g->colorBase + (h64_load_be16(g->sys->rdram + ((a + 6) & 0x7FFFFE)) & 0xFF) :
                fmt == VF_DKR ? a + 6 : a + 12;   // colour, or normal with lighting
        int unlit = 0;
        if (fmt != VF_DKR)
        {
            s = rdf16(g, a + 8);
            t = rdf16(g, a + 10);
        }
        al = rdf8(g, c + 3);
        if (cbfd)
        {
            nx = rdn8(g, g->normalBase + (v0 + i) * 2);
            ny = rdn8(g, g->normalBase + (v0 + i) * 2 + 1);
            nz = rdn8(g, a + 7);
            r = rdf8(g, a + 12);
            gg = rdf8(g, a + 13);
            b = rdf8(g, a + 14);
            unlit = rd8(g, a + 6) >= 0x80;   // the flag halfword's sign
        }
        else if (lighting)
        {
            nx = rdn8(g, c);
            ny = rdn8(g, c + 1);
            nz = rdn8(g, c + 2);
            r = gg = b = 0;
        }
        else
        {
            r = rdf8(g, c);
            gg = rdf8(g, c + 1);
            b = rdf8(g, c + 2);
        }
        // From here on, no memory read can alias what is computed.
        x = px * m[0][0] + py * m[1][0] + pz * m[2][0] + m[3][0];
        y = px * m[0][1] + py * m[1][1] + pz * m[2][1] + m[3][1];
        z = px * m[0][2] + py * m[1][2] + pz * m[2][2] + m[3][2];
        w = px * m[0][3] + py * m[1][3] + pz * m[2][3] + m[3][3];
        if (billboard)
        {
            // Billboards: positions relative to vertex 0 (GLideN64 gSPBillboardVertex),
            // as it is now (vertex 0 itself: its new position, doubled).
            if (v0 + i == 0) { x += x; y += y; z += z; w += w; }
            else { x += g->vtx[0].x; y += g->vtx[0].y; z += g->vtx[0].z; w += g->vtx[0].w; }
        }
        if (cbfd)
        {
            if (!unlit)
            {
                float fr, fg, fb;
                u64 lt0 = h64_prof_now(g->sys);
                cbfd_light(g, x, y, z, nx, ny, nz, ldir, lc, &fr, &fg, &fb);
                g->sys->prof[H64_PROF_GFX_LIGHT] += h64_prof_now(g->sys) - lt0;
                g->sys->prof[H64_PROF_GFX_NLIGHT] += (u64)nLights;
                r *= fr;
                gg *= fg;
                b *= fb;
            }
        }
        else if (lighting)
        {
            r = g->lightCol[nLights][0];
            gg = g->lightCol[nLights][1];
            b = g->lightCol[nLights][2];
            for (l = 0; l < nLights && l < 8; l++)
            {
                float d = nx * ldir[l][0] + ny * ldir[l][1] + nz * ldir[l][2];
                if (d > 0)
                {
                    r += g->lightCol[l][0] * d;
                    gg += g->lightCol[l][1] * d;
                    b += g->lightCol[l][2] * d;
                }
            }
            r = r > 255.0f ? 255.0f : r;
            gg = gg > 255.0f ? 255.0f : gg;
            b = b > 255.0f ? 255.0f : b;
        }
        if (lighting && (gm & GM_TEXGEN))
        {
            float tx = look[0][0] * nx + look[0][1] * ny + look[0][2] * nz;
            float ty = look[1][0] * nx + look[1][1] * ny + look[1][2] * nz;
            // GLideN64's formulas give texels (0..1024 before the
            // G_TEXTURE scale); s and t here are in the vertex's s10.5
            // units, hence x32 (MK64's and GoldenEye's shiny Nintendo
            // logos sampled about one texel: dark).
            if (gm & GM_TEXGEN_LINEAR)
            {
                if (tx < -1.0f) tx = -1.0f;
                if (tx > 1.0f) tx = 1.0f;
                if (ty < -1.0f) ty = -1.0f;
                if (ty > 1.0f) ty = 1.0f;
                s = acosf(-tx) * 325.94931f * 32.0f;
                t = acosf(-ty) * 325.94931f * 32.0f;
            }
            else
            {
                s = (tx + 1.0f) * 512.0f * 32.0f;
                t = (ty + 1.0f) * 512.0f * 32.0f;
            }
        }
        if ((gm & GM_FOG) && w > 0)
        {
            float f = z / w * fogMul + fogOff;
            al = f < 0 ? 0.0f : f > 255.0f ? 255.0f : f;
        }
        if (x > w) clip |= CL_POSX;
        if (x < -w) clip |= CL_NEGX;
        if (y > w) clip |= CL_POSY;
        if (y < -w) clip |= CL_NEGY;
        if (w < 0.01f) clip |= CL_W;
        v->x = x; v->y = y; v->z = z; v->w = w;
        v->r = r; v->g = gg; v->b = b; v->a = al;
        v->s = s * texS;
        v->t = t * texT;
        v->nx = nx; v->ny = ny; v->nz = nz;
        v->clip = clip;
        v->xyOverride = v->zOverride = 0;
    }
}

static void load_vertices(H64Gfx *g, u32 addr, u32 n, u32 v0)
{
    load_vertices_fmt(g, seg_to_phys(g, addr), n, v0, VF_STD);
}

static void modify_vertex(H64Gfx *g, u32 n, u32 where, u32 val)
{
    GfxVtx *v;
    if (n >= GFX_VTX_MAX) return;
    v = &g->vtx[n];
    switch (where)
    {
    case 0x10:   // G_MWO_POINT_RGBA
        v->r = (float)(val >> 24);
        v->g = (float)((val >> 16) & 0xFF);
        v->b = (float)((val >> 8) & 0xFF);
        v->a = (float)(val & 0xFF);
        break;
    case 0x14:   // G_MWO_POINT_ST
        v->s = (float)(s16)(val >> 16);
        v->t = (float)(s16)val;
        break;
    case 0x18:   // G_MWO_POINT_XYSCREEN (1/4 pixel)
        v->xyOverride = 1;
        v->sx = (s16)(val >> 16) / 4.0f;
        v->sy = (s16)val / 4.0f;
        v->clip &= ~(CL_POSX | CL_NEGX | CL_POSY | CL_NEGY);
        break;
    case 0x1C:   // G_MWO_POINT_ZSCREEN (16.16 in G_MAXZ units)
        v->zOverride = 1;
        v->sz = (float)val / 65536.0f * 32.0f;
        break;
    }
}

// ---------------------------------------------------------------- triangles
struct ClipVtx
{
    float x, y, z, w, r, g, b, a, s, t;
};

static void clip_lerp(ClipVtx *o, const ClipVtx *a, const ClipVtx *b, float f)
{
    o->x = a->x + (b->x - a->x) * f;
    o->y = a->y + (b->y - a->y) * f;
    o->z = a->z + (b->z - a->z) * f;
    o->w = a->w + (b->w - a->w) * f;
    o->r = a->r + (b->r - a->r) * f;
    o->g = a->g + (b->g - a->g) * f;
    o->b = a->b + (b->b - a->b) * f;
    o->a = a->a + (b->a - a->a) * f;
    o->s = a->s + (b->s - a->s) * f;
    o->t = a->t + (b->t - a->t) * f;
}

// Clips the polygon against the plane p.x*x + p.y*y + p.z*z + p.w*w >= 0.
static int clip_plane(ClipVtx *in, int n, ClipVtx *out, const float p[4])
{
    int i, m = 0;
    for (i = 0; i < n; i++)
    {
        const ClipVtx *a = &in[i], *b = &in[(i + 1) % n];
        float da = p[0] * a->x + p[1] * a->y + p[2] * a->z + p[3] * a->w;
        float db = p[0] * b->x + p[1] * b->y + p[2] * b->z + p[3] * b->w;
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0))
        {
            clip_lerp(&out[m], a, b, da / (da - db));
            m++;
        }
    }
    return m;
}

static void project_iw(H64Gfx *g, const ClipVtx *c, float iw, H64RenderVertex *o)
{
    float z;
    o->x = g->vtrans[0] + g->vscale[0] * c->x * iw;
    o->y = g->vtrans[1] - g->vscale[1] * c->y * iw;
    // Not clamped per vertex: the RDP clamps Z per pixel. With the ".NoN"
    // microcodes (no near clipping) a vertex just in front of the eye gets a Z
    // out of range, and clamping it skewed the whole triangle's depth (OoT's
    // Kokiri paths, decals on the ground, failed their depth test at some
    // camera angles). The triangle setup saturates to its s15.16 range.
    z = (g->vtrans[2] + g->vscale[2] * c->z * iw) * 32.0f;
    o->z = z;
    o->invw = iw;
    o->r = c->r;
    o->g = c->g;
    o->b = c->b;
    o->a = c->a;
    o->s = c->s;
    o->t = c->t;
}

static void project(H64Gfx *g, const ClipVtx *c, H64RenderVertex *o)
{
    project_iw(g, c, 1.0f / c->w, o);
}

static void triangle(H64Gfx *g, u32 i0, u32 i1, u32 i2)
{
    const GfxVtx *v[3];
    ClipVtx poly[2][16];
    H64RenderVertex sv[16];
    u32 gm = geom(g), flags = 0;
    int n = 3, cur = 0, k, needClip = 0;
    float area = 0, iw[3];
    g->nTris++;
    if (i0 >= GFX_VTX_MAX || i1 >= GFX_VTX_MAX || i2 >= GFX_VTX_MAX) return;
    v[0] = &g->vtx[i0];
    v[1] = &g->vtx[i1];
    v[2] = &g->vtx[i2];
    // Entirely outside one plane (screen sides at the clip ratio, or behind the eye).
    if (v[0]->clip & v[1]->clip & v[2]->clip & CL_W) return;
    for (k = 0; k < 3; k++)
    {
        ClipVtx *c = &poly[0][k];
        c->x = v[k]->x;
        c->y = v[k]->y;
        c->z = v[k]->z;
        c->w = v[k]->w;
        c->r = v[k]->r;
        c->g = v[k]->g;
        c->b = v[k]->b;
        c->a = v[k]->a;
        c->s = v[k]->s;
        c->t = v[k]->t;
        if (v[k]->xyOverride || v[k]->zOverride)
        {
            if (c->w <= 0) c->w = 1.0f;
        }
        else if (c->w < 0.0001f || (!g->uc->noNear && c->z < -c->w))
            needClip = 1;
    }
    if (!(gm & GM_SMOOTH))
        for (k = 1; k < 3; k++)
        {
            poly[0][k].r = poly[0][0].r;
            poly[0][k].g = poly[0][0].g;
            poly[0][k].b = poly[0][0].b;
            poly[0][k].a = poly[0][0].a;
        }
    if (!needClip)
    {
        // Keep the projected positions inside the RDP's range (1 / w kept
        // for project: one division a vertex, ~30 cycles each on the Xbox 360).
        for (k = 0; k < 3; k++)
        {
            float sx, sy;
            iw[k] = 1.0f / poly[0][k].w;
            sx = g->vtrans[0] + g->vscale[0] * poly[0][k].x * iw[k];
            sy = g->vtrans[1] - g->vscale[1] * poly[0][k].y * iw[k];
            if (sx < -1000.0f || sx > 2000.0f || sy < -1000.0f || sy > 2000.0f) needClip = 2;
        }
    }
    if (needClip)
    {
        // Planes, as p . (x, y, z, w) >= 0: w > 0 (just in front of the eye),
        // the near plane z >= -w unless the microcode does not clip it, and a
        // guard band keeping screen positions in [-1000, 2000].
        float p[6][4];
        int j, planes = 0;
        p[planes][0] = 0; p[planes][1] = 0; p[planes][2] = 0; p[planes][3] = 1.0f; planes++;
        if (!g->uc->noNear) { p[planes][0] = 0; p[planes][1] = 0; p[planes][2] = 1.0f; p[planes][3] = 1.0f; planes++; }
        p[planes][0] = g->vscale[0]; p[planes][1] = 0; p[planes][2] = 0; p[planes][3] = g->vtrans[0] + 1000.0f; planes++;
        p[planes][0] = -g->vscale[0]; p[planes][1] = 0; p[planes][2] = 0; p[planes][3] = 2000.0f - g->vtrans[0]; planes++;
        p[planes][0] = 0; p[planes][1] = -g->vscale[1]; p[planes][2] = 0; p[planes][3] = g->vtrans[1] + 1000.0f; planes++;
        p[planes][0] = 0; p[planes][1] = g->vscale[1]; p[planes][2] = 0; p[planes][3] = 2000.0f - g->vtrans[1]; planes++;
        for (j = 0; j < planes && n >= 3; j++)
        {
            n = clip_plane(poly[cur], n, poly[cur ^ 1], p[j]);
            cur ^= 1;
            if (j == 0)
                for (k = 0; k < n; k++)
                    if (poly[cur][k].w < 0.0001f) poly[cur][k].w = 0.0001f;
        }
        if (n < 3) return;
    }
    for (k = 0; k < n; k++)
    {
        if (!needClip) project_iw(g, &poly[cur][k], iw[k], &sv[k]);
        else project(g, &poly[cur][k], &sv[k]);
        if (!needClip && k < 3)
        {
            if (v[k]->xyOverride) { sv[k].x = v[k]->sx; sv[k].y = v[k]->sy; }
            if (v[k]->zOverride) sv[k].z = v[k]->sz;
        }
    }
    // Facing: visually counter-clockwise triangles are front faces; screen y
    // points down, so their signed area is negative here.
    for (k = 0; k < n; k++)
    {
        const H64RenderVertex *a = &sv[k], *b = &sv[k + 1 < n ? k + 1 : 0];
        area += a->x * b->y - b->x * a->y;
    }
    if (area == 0) return;
    if ((gm & GM_CULL_BACK) && area > 0) return;
    if ((gm & GM_CULL_FRONT) && area < 0) return;

    if (gm & GM_ZBUFFER) flags |= H64_TRI_ZBUFFER;
    if (gm & GM_SHADE) flags |= H64_TRI_SHADE;
    if (g->texOn) flags |= H64_TRI_TEXTURE;
    for (k = 1; k + 1 < n; k++)
        out_tri(g, &sv[0], &sv[k], &sv[k + 1], flags);
}

// Culls the display list when every vertex in [v0, vn] is outside the same plane.
static int cull_vertices(H64Gfx *g, u32 v0, u32 vn)
{
    u32 i, clip = CL_POSX | CL_NEGX | CL_POSY | CL_NEGY | CL_W;
    if (vn < v0) { u32 t = v0; v0 = vn; vn = t; }
    if (vn >= GFX_VTX_MAX) return 1;
    for (i = v0; i <= vn; i++)
    {
        clip &= g->vtx[i].clip;
        if (!clip) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- commands
static void end_dl(H64Gfx *g)
{
    if (g->pci > 0) g->pci--;
    else g->halt = 1;
}

static void call_dl(H64Gfx *g, u32 addr, int push)
{
    u32 phys = seg_to_phys(g, addr);
    if (push)
    {
        if (g->pci < GFX_PC_MAX - 1) g->pc[++g->pci] = phys;
        else g->abort = 1;
    }
    else
        g->pc[g->pci] = phys;
}

static void do_matrix(H64Gfx *g, u32 addr, int projection, int load, int push)
{
    float m[4][4];
    load_matrix(g, seg_to_phys(g, addr), m);
    if (projection)
    {
        if (load) memcpy(g->proj, m, sizeof(m));
        else mat_mul(g->proj, m, g->proj);
    }
    else
    {
        if (push && g->mvi < g->stackSize - 1)
        {
            memcpy(g->mv[g->mvi + 1], g->mv[g->mvi], sizeof(m));
            g->mvi++;
        }
        if (load) memcpy(g->mv[g->mvi], m, sizeof(m));
        else mat_mul(g->mv[g->mvi], m, g->mv[g->mvi]);
    }
    g->combinedValid = 0;
    g->forcedCombined = 0;
}

static void pop_matrix(H64Gfx *g, u32 n)
{
    if ((u32)g->mvi >= n) g->mvi -= (int)n;
    else g->mvi = 0;
    if (!g->forcedCombined) g->combinedValid = 0;
}

static void force_matrix(H64Gfx *g, u32 addr)
{
    load_matrix(g, seg_to_phys(g, addr), g->combined);
    g->combinedValid = 1;
    g->forcedCombined = 1;
}

static void set_viewport(H64Gfx *g, u32 addr)
{
    u32 a = seg_to_phys(g, addr);
    g->vscale[0] = rd16(g, a) / 4.0f;
    g->vscale[1] = rd16(g, a + 2) / 4.0f;
    g->vscale[2] = rd16(g, a + 4);
    g->vscale[3] = rd16(g, a + 6);
    g->vtrans[0] = rd16(g, a + 8) / 4.0f;
    g->vtrans[1] = rd16(g, a + 10) / 4.0f;
    g->vtrans[2] = rd16(g, a + 12);
    g->vtrans[3] = rd16(g, a + 14);
}

static void set_light(H64Gfx *g, u32 addr, int n)
{
    u32 a = seg_to_phys(g, addr);
    if (n < 0 || n > 8) return;
    g->lightCol[n][0] = rd8(g, a);
    g->lightCol[n][1] = rd8(g, a + 1);
    g->lightCol[n][2] = rd8(g, a + 2);
    g->lightDir[n][0] = (s8)rd8(g, a + 8);
    g->lightDir[n][1] = (s8)rd8(g, a + 9);
    g->lightDir[n][2] = (s8)rd8(g, a + 10);
}

static void set_lookat(H64Gfx *g, u32 addr, int n)
{
    u32 a = seg_to_phys(g, addr);
    float x = (s8)rd8(g, a + 8), y = (s8)rd8(g, a + 9), z = (s8)rd8(g, a + 10);
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0) { x /= len; y /= len; z /= len; }
    g->lookat[n][0] = x;
    g->lookat[n][1] = y;
    g->lookat[n][2] = z;
}

static void set_light_color(H64Gfx *g, int n, u32 col)
{
    if (n < 0 || n > 8) return;
    g->lightCol[n][0] = (float)(col >> 24);
    g->lightCol[n][1] = (float)((col >> 16) & 0xFF);
    g->lightCol[n][2] = (float)((col >> 8) & 0xFF);
}

static void set_texture(H64Gfx *g, u32 w1, u32 level, u32 tile, u32 on)
{
    g->texOn = on;
    if (!on) return;
    g->texScaleS = (w1 >> 16) / 65536.0f;
    g->texScaleT = (w1 & 0xFFFF) / 65536.0f;
    if (g->texScaleS == 0) g->texScaleS = 1.0f;
    if (g->texScaleT == 0) g->texScaleT = 1.0f;
    g->texLevel = level;
    g->texTile = tile;
}

static void set_othermode(H64Gfx *g, int high, u32 shift, u32 len, u32 data)
{
    u32 mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
    if (high) g->omH = (g->omH & ~mask) | (data & mask);
    else g->omL = (g->omL & ~mask) | (data & mask);
    out_othermode(g);
}

static void insert_matrix(H64Gfx *g, u32 where, u32 num)
{
    float *m;
    u32 addr = (where + 0x80) & 0xFFFF, idx, i;
    if (where & 3) return;
    if (addr < 0x40) m = &g->mv[g->mvi][0][0];
    else if (addr < 0x80) { m = &g->proj[0][0]; addr -= 0x40; }
    else if (addr < 0xC0) { update_combined(g); m = &g->combined[0][0]; addr -= 0x80; g->forcedCombined = 1; }
    else return;
    idx = addr < 0x20 ? addr >> 1 : (addr - 0x20) >> 1;
    for (i = 0; i < 2; i++)
    {
        u16 part = (u16)(i == 0 ? num >> 16 : num);
        s32 fixed = (s32)floor(m[idx + i] * 65536.0f + 0.5f);
        if (addr < 0x20) fixed = (s32)(((u32)part << 16) | ((u32)fixed & 0xFFFF));
        else fixed = (s32)(((u32)fixed & 0xFFFF0000u) | part);
        m[idx + i] = fixed / 65536.0f;
    }
    if (m != &g->combined[0][0] && !g->forcedCombined) g->combinedValid = 0;
}

// An RDP command embedded in the display list (opcode 0xC0..0xFF).
static void rdp_passthrough(H64Gfx *g, u32 w0, u32 w1)
{
    u32 op = w0 >> 24;
    switch (op)
    {
    case 0xE4: case 0xE5:   // TEXRECT(FLIP): the next two commands carry (s, t) and (dsdx, dtdy)
    {
        u32 a = g->pc[g->pci];
        u64 w[2];
        u32 h1 = rd32(g, a + 4), h2 = rd32(g, a + 12);
        g->pc[g->pci] += 16;
        w[0] = ((u64)w0 << 32) | w1;
        w[1] = ((u64)h1 << 32) | h2;
        out_rdp(g, w, 2);
        return;
    }
    case 0xFD: case 0xFE: case 0xFF:   // SETTIMG / SETZIMG / SETCIMG: segmented address
        out_rdp1(g, w0, seg_to_phys(g, w1));
        return;
    case 0xEF:   // SETOTHERMODE
        g->omH = w0 & 0x00FFFFFF;
        g->omL = w1;
        out_rdp1(g, w0, w1);
        return;
    case 0xE9:   // FULLSYNC
        g->fullSync = 1;
        out_rdp1(g, w0, w1);
        return;
    default:
        if (op >= 0xC8 && op <= 0xCF)
        {
            // Raw RDP triangles are not expected in display lists.
            if (!(g->warned & 1)) { H64_WARN("[gfx] raw RDP triangle in a display list (ignored)"); g->warned |= 1; }
            return;
        }
        out_rdp1(g, w0, w1);
        return;
    }
}

static int load_ucode(H64Gfx *g, u32 start, u32 dstart, u32 dsize)
{
    const GfxUcode *u = find_ucode(g, start & 0x7FFFFF, dstart & 0x7FFFFF, dsize);   // KSEG0 or physical
    if (u->type == UC_NONE) return 0;
    g->uc = u;
    return 1;
}

static void f3d_moveword(H64Gfx *g, u32 w0, u32 w1)
{
    u32 index = w0 & 0xFF, offset = (w0 >> 8) & 0xFFFF;
    switch (index)
    {
    case 0x00: insert_matrix(g, offset, w1); break;
    case 0x02:
        g->numLights = w1 >= 0x80000020u ? (int)((w1 - 0x80000000u) >> 5) - 1 : 0;
        if (g->numLights > 8) g->numLights = 8;
        if (g->numLights < 0) g->numLights = 0;
        break;
    case 0x04: break;   // G_MW_CLIP
    case 0x06: g->segments[(offset >> 2) & 0xF] = w1 & 0x00FFFFFF; break;
    case 0x08:
        g->fogMul = (float)(s16)(w1 >> 16);
        g->fogOff = (float)(s16)w1;
        break;
    case 0x0A:
        if ((offset & 0x1F) == 0) set_light_color(g, (int)(offset >> 5), w1);
        break;
    case 0x0C: modify_vertex(g, offset / 40, offset % 40, w1); break;
    default: break;
    }
}

// A triangle with its own texture coordinates (Diddy Kong Racing's DMA
// triangles carry s, t per corner).
static void triangle_st(H64Gfx *g, u32 i0, u32 i1, u32 i2, const float st[6])
{
    u32 idx[3];
    float saved[3][2];
    int k;
    idx[0] = i0; idx[1] = i1; idx[2] = i2;
    if (i0 >= GFX_VTX_MAX || i1 >= GFX_VTX_MAX || i2 >= GFX_VTX_MAX) return;
    for (k = 0; k < 3; k++) { saved[k][0] = g->vtx[idx[k]].s; saved[k][1] = g->vtx[idx[k]].t; }
    for (k = 0; k < 3; k++) { g->vtx[idx[k]].s = st[k * 2]; g->vtx[idx[k]].t = st[k * 2 + 1]; }
    triangle(g, i0, i1, i2);
    for (k = 2; k >= 0; k--) { g->vtx[idx[k]].s = saved[k][0]; g->vtx[idx[k]].t = saved[k][1]; }
}

// Commands that differ in Rare's microcodes (GLideN64 F3DGOLDEN.cpp, F3DPD.cpp,
// F3DDKR.cpp, gSP.cpp: studied and rewritten). Returns 0 for the commands
// they share with F3D.
static int run_rare(H64Gfx *g, u32 w0, u32 w1)
{
    u32 cmd = w0 >> 24;
    int rare = g->uc->rare;
    if (rare == RARE_GOLDEN || rare == RARE_PD)
    {
        if (cmd == 0xB1)   // G_TRIX: up to four triangles, 4-bit vertex indices
        {
            while (w1 != 0)
            {
                u32 a = w1 & 0xF, b = (w1 >> 4) & 0xF, c = w0 & 0xF;
                w1 >>= 8;
                w0 >>= 4;
                triangle(g, a, b, c);
            }
            return 1;
        }
        if (rare == RARE_GOLDEN && cmd == 0xBD) { f3d_moveword(g, w0, w1); return 1; }
        if (rare == RARE_PD && cmd == 0x04)
        {
            load_vertices_fmt(g, seg_to_phys(g, w1), ((w0 >> 20) & 0xF) + 1, (w0 >> 16) & 0xF, VF_PD);
            return 1;
        }
        if (rare == RARE_PD && cmd == 0x07) { g->colorBase = seg_to_phys(g, w1); return 1; }   // G_VTXCOLORBASE
        return 0;
    }
    // Diddy Kong Racing, Jet Force Gemini, Mickey's Speedway USA
    switch (cmd)
    {
    case 0x01:   // G_DMA_MTX: one of four model-view slots, projection identity
    {
        float m[4][4];
        u32 index = (w0 >> 16) & 0xF, multiply;
        if ((w0 & 0xFFFF) != 64) return 1;
        if (index == 0) { index = (w0 >> 22) & 3; multiply = 0; }   // DKR
        else multiply = (w0 >> 23) & 1;                               // JFG
        load_matrix(g, (g->dmaMtx + seg_to_phys(g, w1)) & 0x7FFFFF, m);
        g->mvi = (int)index;
        if (multiply) mat_mul(g->mv[index], m, g->mv[0]);
        else memcpy(g->mv[index], m, sizeof(m));
        mat_identity(g->proj);
        g->combinedValid = 0;
        return 1;
    }
    case 0x02:   // G_DMA_TEX_OFFSET: a table of texture address offsets
        g->texOffset = seg_to_phys(g, w1);
        g->texShift = 0;
        g->texCount = 0;
        return 1;
    case 0x04:   // G_DMA_VTX
    {
        u32 n = (w0 >> 19) & 0x1F;
        if (rare == RARE_DKR) n++;
        if (w0 & 0x10000) { if (g->billboard) g->vertexi = 1; }
        else g->vertexi = 0;
        load_vertices_fmt(g, (g->dmaVtx + seg_to_phys(g, w1)) & 0x7FFFFF, n, (u32)g->vertexi + ((w0 >> 9) & 0x1F), VF_DKR);
        g->vertexi += (int)n;
        return 1;
    }
    case 0x05:   // G_DMA_TRI: 16-byte triangles with their own s, t
    {
        u32 n = ((w0 >> 20) & 0xF) + 1, a = seg_to_phys(g, w1), i;
        g->texOn = (w0 >> 16) & 0xF;
        for (i = 0; i < n; i++, a += 16)
        {
            u32 w = rd32(g, a);
            float st[6];
            int k;
            g->geomRaw &= ~0x3000u;
            if (!(w & 0x40000000u)) g->geomRaw |= g->vscale[0] > 0 ? 0x2000u : 0x1000u;   // cull back/front
            for (k = 0; k < 3; k++)
            {
                st[k * 2] = rd16(g, a + 4 + k * 4) * g->texScaleS;
                st[k * 2 + 1] = rd16(g, a + 6 + k * 4) * g->texScaleT;
            }
            triangle_st(g, (w >> 16) & 0xFF, (w >> 8) & 0xFF, w & 0xFF, st);
        }
        g->vertexi = 0;
        return 1;
    }
    case 0x07:   // G_DMA_DL: the next `count` commands of another list
        if (g->pci < GFX_PC_MAX - 1)
        {
            g->pc[++g->pci] = seg_to_phys(g, w1);
            g->dlCount = (int)((w0 >> 16) & 0xFF) + 1;
        }
        return 1;
    case 0xBF:   // G_DMA_OFFSETS
        g->dmaMtx = w0 & 0xFFFFFF;
        g->dmaVtx = w1 & 0xFFFFFF;
        return 1;
    case 0xBC:   // G_MOVEWORD: billboard flag, model-view slot
        if ((w0 & 0xFF) == 0x02) { g->billboard = (int)(w1 & 1); return 1; }
        if ((w0 & 0xFF) == 0x0A) { g->mvi = (int)((w1 >> 6) & 3); g->combinedValid = 0; return 1; }
        return 0;
    case 0xFD:   // SETTIMG: RGBA images move by the offset table's current entry
        g->timgW0 = w0;
        g->timgW1 = w1;
        if (g->texOffset)
        {
            if (((w0 >> 21) & 7) == 0)
            {
                g->texShift = (u32)(u16)rd16(g, g->texOffset + g->texCount * 2);
                rdp_passthrough(g, w0, seg_to_phys(g, w1) + g->texShift);
                return 1;
            }
            g->texOffset = g->texShift = g->texCount = 0;
        }
        return 0;
    case 0xF3:   // LOADBLOCK
        if (g->texOffset)
        {
            u32 lrs = (w1 >> 12) & 0xFFF;
            if (g->texShift % (((lrs >> 2) + 1) << 3))
            {
                rdp_passthrough(g, g->timgW0, seg_to_phys(g, g->timgW1));   // the unshifted image after all
                g->texOffset = g->texShift = g->texCount = 0;
            }
            else
                g->texCount++;
        }
        return 0;
    case 0x03: case 0x06: case 0x00: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
    case 0xB6: case 0xB7: case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBE:
        return 0;
    default:
        return cmd >= 0xC0 ? 0 : 1;   // F3D commands these microcodes do not have
    }
}

// ---- F3D / F3DEX (the 0xB0..0xBF immediate commands share most encodings)
static void run_f3d(H64Gfx *g, u32 w0, u32 w1)
{
    u32 cmd = w0 >> 24;
    int ex = g->uc->type == UC_F3DEX;
    if (g->uc->rare && run_rare(g, w0, w1)) return;
    switch (cmd)
    {
    case 0x00: break;   // G_SPNOOP
    case 0x01:          // G_MTX
    {
        u32 p = (w0 >> 16) & 0xFF;
        do_matrix(g, w1, p & 1, p & 2, p & 4);
        break;
    }
    case 0x03:          // G_MOVEMEM
    {
        u32 idx = (w0 >> 16) & 0xFF;
        if (idx == 0x80) set_viewport(g, w1);
        else if (idx == 0x82) set_lookat(g, w1, 1);
        else if (idx == 0x84) set_lookat(g, w1, 0);
        else if (idx >= 0x86 && idx <= 0x94) set_light(g, w1, (int)(idx - 0x86) / 2);
        else if (idx == 0x9E) { force_matrix(g, w1); g->pc[g->pci] += 24; }
        break;
    }
    case 0x04:          // G_VTX
        if (ex) load_vertices(g, w1, (w0 >> 10) & 0x3F, (w0 >> 17) & 0x7F);
        else load_vertices(g, w1, ((w0 >> 20) & 0xF) + 1, (w0 >> 16) & 0xF);
        break;
    case 0x06:          // G_DL
        call_dl(g, w1, ((w0 >> 16) & 0xFF) == 0);
        break;
    case 0xAF:          // G_LOAD_UCODE (F3DEX)
        if (!ex || !load_ucode(g, w1, g->half1, (w0 & 0xFFFF) + 1)) g->abort = 1;
        break;
    case 0xB0:          // G_BRANCH_Z (F3DEX)
        if (ex)
        {
            u32 n = (w0 >> 1) & 0x7FF;
            if (n < GFX_VTX_MAX)
            {
                const GfxVtx *v = &g->vtx[n];
                u32 z = v->w != 0 ? (u32)(s32)(v->z / v->w * 1023.0f) : 0x400;
                if (z > 0x3FF || z <= ((w1 >> 16) & 0xFFFF)) g->pc[g->pci] = seg_to_phys(g, g->half1);
            }
        }
        break;
    case 0xB1:          // G_TRI2 (F3DEX)
        if (ex)
        {
            triangle(g, (w0 >> 17) & 0x7F, (w0 >> 9) & 0x7F, (w0 >> 1) & 0x7F);
            triangle(g, (w1 >> 17) & 0x7F, (w1 >> 9) & 0x7F, (w1 >> 1) & 0x7F);
        }
        break;
    case 0xB2:          // G_MODIFYVTX (F3DEX), G_RDPHALF_CONT (F3D)
        if (ex) modify_vertex(g, (w0 & 0xFFFF) >> 1, (w0 >> 16) & 0xFF, w1);
        break;
    case 0xB3: break;   // G_RDPHALF_2 (read with TEXRECT)
    case 0xB4: g->half1 = w1; break;   // G_RDPHALF_1
    case 0xB5:          // G_QUAD
    {
        u32 a = (w1 >> 25) & 0x7F, b = (w1 >> 17) & 0x7F, c = (w1 >> 9) & 0x7F, d = (w1 >> 1) & 0x7F;
        triangle(g, a, b, c);
        triangle(g, a, c, d);
        break;
    }
    case 0xB6: g->geomRaw &= ~w1; break;   // G_CLEARGEOMETRYMODE
    case 0xB7: g->geomRaw |= w1; break;    // G_SETGEOMETRYMODE
    case 0xB8: end_dl(g); break;           // G_ENDDL
    case 0xB9: set_othermode(g, 0, (w0 >> 8) & 0xFF, w0 & 0xFF, w1); break;
    case 0xBA: set_othermode(g, 1, (w0 >> 8) & 0xFF, w0 & 0xFF, w1); break;
    case 0xBB: set_texture(g, w1, (w0 >> 11) & 7, (w0 >> 8) & 7, w0 & 0xFF); break;
    case 0xBC: f3d_moveword(g, w0, w1); break;   // G_MOVEWORD
    case 0xBD: if (w1 == 0) pop_matrix(g, 1); break;   // G_POPMTX (modelview)
    case 0xBE:          // G_CULLDL
        if (ex) { if (cull_vertices(g, (w0 & 0xFFFF) >> 1, (w1 & 0xFFFF) >> 1)) end_dl(g); }
        else if (cull_vertices(g, (w0 & 0xFFFFFF) / 40, w1 / 40 - 1)) end_dl(g);
        break;
    case 0xBF:          // G_TRI1
        if (ex) triangle(g, (w1 >> 17) & 0x7F, (w1 >> 9) & 0x7F, (w1 >> 1) & 0x7F);
        else
        {
            u32 a = ((w1 >> 16) & 0xFF) / 10, b = ((w1 >> 8) & 0xFF) / 10, c = (w1 & 0xFF) / 10;
            u32 flag = w1 >> 24;
            if (flag == 1) triangle(g, b, c, a);
            else if (flag == 2) triangle(g, c, a, b);
            else triangle(g, a, b, c);
        }
        break;
    default:
        if (cmd >= 0xC0) rdp_passthrough(g, w0, w1);
        else if (!(g->warned & 2)) { H64_WARN("[gfx] F3D command %02X not handled", cmd); g->warned |= 2; }
        break;
    }
}

// Conker's Bad Fur Day (GLideN64 F3DEX2CBFD.cpp and gSP.cpp: studied and
// rewritten). Returns 0 for the commands it shares with F3DEX2.
static int run_cbfd(H64Gfx *g, u32 w0, u32 w1)
{
    u32 cmd = w0 >> 24;
    if (cmd >= 0x10 && cmd <= 0x1F)   // four triangles, 5-bit vertex indices
    {
        triangle(g, (w0 >> 23) & 31, (w0 >> 18) & 31, (((w0 >> 15) & 7) << 2) | (w1 >> 30));
        triangle(g, (w0 >> 10) & 31, (w0 >> 5) & 31, w0 & 31);
        triangle(g, (w1 >> 25) & 31, (w1 >> 20) & 31, (w1 >> 15) & 31);
        triangle(g, (w1 >> 10) & 31, (w1 >> 5) & 31, w1 & 31);
        return 1;
    }
    switch (cmd)
    {
    case 0x01:   // G_VTX: the same encoding, Conker's vertex
    {
        u32 n = (w0 >> 12) & 0xFF;
        load_vertices_fmt(g, seg_to_phys(g, w1), n, ((w0 >> 1) & 0x7F) - n, VF_CBFD);
        return 1;
    }
    case 0xDD: g->advLighting = 1; return 1;   // G_LOAD_UCODE: switches to the advanced lighting
    case 0xDC:   // G_MOVEMEM
        switch (w0 & 0xFF)
        {
        case 8: set_viewport(g, w1); break;
        case 10:   // lights are 48 bytes; the first two are the lookats
        {
            u32 n = ((w0 >> 5) & 0x3FFF) / 48;
            if (n < 2) set_lookat(g, w1, (int)n);
            else if (n - 2 < 10)
            {
                u32 a = seg_to_phys(g, w1);
                n -= 2;
                set_light(g, w1, (int)n);
                g->lightPos[n][0] = rd16(g, a + 32);
                g->lightPos[n][1] = rd16(g, a + 34);
                g->lightPos[n][2] = rd16(g, a + 36);
                g->lightCa[n] = rd8(g, a + 12) / 16.0f;
            }
            break;
        }
        case 14: g->normalBase = seg_to_phys(g, w1); break;   // G_MV_NORMALES
        default: break;
        }
        return 1;
    case 0xDB:   // G_MOVEWORD
        switch ((w0 >> 16) & 0xFF)
        {
        case 0x02: g->numLights = (int)(w1 / 48) > 8 ? 8 : (int)(w1 / 48); return 1;
        case 0x06: case 0x08: return 0;   // segment, fog: as F3DEX2
        case 0x10:   // G_MW_COORD_MOD
            if (!(w0 & 8))
            {
                u32 idx = (w0 >> 1) & 3, pos = w0 & 0x30;
                float *cm = g->coordMod;
                if (pos == 0) { cm[idx] = (s16)(w1 >> 16); cm[1 + idx] = (s16)w1; }
                else if (pos == 0x10)
                {
                    cm[4 + idx] = (w1 >> 16) / 65536.0f;
                    cm[5 + idx] = (w1 & 0xFFFF) / 65536.0f;
                    cm[12 + idx] = cm[idx] + cm[4 + idx];
                    cm[13 + idx] = cm[1 + idx] + cm[5 + idx];
                }
                else if (pos == 0x20) { cm[8 + idx] = (s16)(w1 >> 16); cm[9 + idx] = (s16)w1; }
            }
            return 1;
        default: return 1;
        }
    default:
        return 0;
    }
}

// ---- F3DEX2 / F3DZEX
static void run_f3dex2(H64Gfx *g, u32 w0, u32 w1)
{
    u32 cmd = w0 >> 24;
    if (g->uc->rare == RARE_CBFD && run_cbfd(g, w0, w1)) return;
    switch (cmd)
    {
    case 0x00: break;   // G_NOOP
    case 0x01:          // G_VTX
    {
        u32 n = (w0 >> 12) & 0xFF;
        load_vertices(g, w1, n, ((w0 >> 1) & 0x7F) - n);
        break;
    }
    case 0x02: modify_vertex(g, (w0 & 0xFFFF) >> 1, (w0 >> 16) & 0xFF, w1); break;   // G_MODIFYVTX
    case 0x03: if (cull_vertices(g, (w0 & 0xFFFF) >> 1, (w1 & 0xFFFF) >> 1)) end_dl(g); break;   // G_CULLDL
    case 0x04:          // G_BRANCH_Z, or G_BRANCH_W on F3DZEX
    {
        if (g->uc->zex)
        {
            u32 n = (w0 >> 1) & 0x7F;
            if (n < GFX_VTX_MAX && g->vtx[n].w < (float)w1) g->pc[g->pci] = seg_to_phys(g, g->half1);
        }
        else
        {
            u32 n = (w0 >> 1) & 0x7FF;
            if (n < GFX_VTX_MAX)
            {
                const GfxVtx *v = &g->vtx[n];
                u32 z = v->w != 0 ? (u32)(s32)(v->z / v->w * 1023.0f) : 0x400;
                if (z > 0x3FF || z <= ((w1 >> 16) & 0xFFFF)) g->pc[g->pci] = seg_to_phys(g, g->half1);
            }
        }
        break;
    }
    case 0x05: triangle(g, (w0 >> 17) & 0x7F, (w0 >> 9) & 0x7F, (w0 >> 1) & 0x7F); break;   // G_TRI1
    case 0x06: case 0x07:   // G_TRI2, G_QUAD
        triangle(g, (w0 >> 17) & 0x7F, (w0 >> 9) & 0x7F, (w0 >> 1) & 0x7F);
        triangle(g, (w1 >> 17) & 0x7F, (w1 >> 9) & 0x7F, (w1 >> 1) & 0x7F);
        break;
    case 0x08: break;   // G_LINE3D (not drawn)
    case 0xD3: case 0xD4: case 0xD5: case 0xD6: break;   // G_SPECIAL_*, G_DMA_IO
    case 0xD7: set_texture(g, w1, (w0 >> 11) & 7, (w0 >> 8) & 7, (w0 >> 1) & 0x7F); break;
    case 0xD8: pop_matrix(g, w1 >> 6); break;   // G_POPMTX
    case 0xD9: g->geomRaw = (g->geomRaw & (w0 & 0x00FFFFFF)) | w1; break;   // G_GEOMETRYMODE
    case 0xDA:          // G_MTX
    {
        u32 p = (w0 & 0xFF) ^ 1;   // G_MTX_PUSH is inverted
        do_matrix(g, w1, p & 4, p & 2, p & 1);
        break;
    }
    case 0xDB:          // G_MOVEWORD
    {
        u32 index = (w0 >> 16) & 0xFF, offset = w0 & 0xFFFF;
        switch (index)
        {
        case 0x00: insert_matrix(g, offset, w1); break;
        case 0x02: g->numLights = (int)(w1 / 24); if (g->numLights > 8) g->numLights = 8; break;
        case 0x04: break;
        case 0x06: g->segments[(offset >> 2) & 0xF] = w1 & 0x00FFFFFF; break;
        case 0x08:
            g->fogMul = (float)(s16)(w1 >> 16);
            g->fogOff = (float)(s16)w1;
            break;
        case 0x0A: if (offset % 24 == 0) set_light_color(g, (int)(offset / 24), w1); break;
        case 0x0C:   // G_MW_FORCEMTX
            if (w1 == 0) { g->forcedCombined = 0; g->combinedValid = 0; }
            else g->forcedCombined = 1;
            break;
        default: break;
        }
        break;
    }
    case 0xDC:          // G_MOVEMEM
    {
        u32 idx = w0 & 0xFF;
        if (idx == 8) set_viewport(g, w1);
        else if (idx == 14) { force_matrix(g, w1); g->pc[g->pci] += 8; }
        else if (idx == 10)
        {
            u32 n = (((w0 >> 8) & 0xFF) * 8) / 24;
            if (n < 2) set_lookat(g, w1, (int)n);
            else set_light(g, w1, (int)n - 2);
        }
        break;
    }
    case 0xDD:          // G_LOAD_UCODE
        if (!load_ucode(g, w1, g->half1, (w0 & 0xFFFF) + 1)) g->abort = 1;
        break;
    case 0xDE: call_dl(g, w1, ((w0 >> 16) & 0xFF) == 0); break;   // G_DL
    case 0xDF: end_dl(g); break;                                   // G_ENDDL
    case 0xE0: break;                                              // G_SPNOOP
    case 0xE1: g->half1 = w1; break;                               // G_RDPHALF_1
    case 0xE2: case 0xE3:   // G_SETOTHERMODE_L / _H
    {
        u32 len = (w0 & 0xFF) + 1;
        s32 shift = 32 - (s32)((w0 >> 8) & 0xFF) - (s32)len;
        if (shift < 0) shift = 0;
        set_othermode(g, cmd == 0xE3, (u32)shift, len, w1);
        break;
    }
    case 0xF1: break;   // G_RDPHALF_2 (read with TEXRECT)
    default:
        if (cmd >= 0xC0) rdp_passthrough(g, w0, w1);
        else if (!(g->warned & 4)) { H64_WARN("[gfx] F3DEX2 command %02X not handled", cmd); g->warned |= 4; }
        break;
    }
}

// ---- S2DEX2 (subset): the commands OoT's prerendered backgrounds use. Any
// other S2DEX command leaves the task to the LLE RSP.
//
// G_BG_COPY draws a picture from RDRAM 1:1 in copy mode. The microcode does
// it with LoadTile/TEXRECT strips (gs2dex.h, uObjBg); the same RDP commands
// are emitted here: for each band of lines that fits in TMEM, the texels go
// to TMEM through tile 7 and a copy-mode rectangle draws them with tile 0.
// The picture wraps at imageW/imageH; a frame starting above or left of the
// screen is clipped there (the scissor clips the rest).
static void bg_copy(H64Gfx *g, u32 addr)
{
    u32 a = seg_to_phys(g, addr);
    s32 imageX = (u16)rd16(g, a + 0) >> 5, imageW = (u16)rd16(g, a + 2) >> 2;
    s32 frameX = rd16(g, a + 4) >> 2, frameW = (u16)rd16(g, a + 6) >> 2;
    s32 imageY = (u16)rd16(g, a + 8) >> 5, imageH = (u16)rd16(g, a + 10) >> 2;
    s32 frameY = rd16(g, a + 12) >> 2, frameH = (u16)rd16(g, a + 14) >> 2;
    u32 ptr = seg_to_phys(g, rd32(g, a + 16));
    u32 fmt = rd8(g, a + 22), siz = rd8(g, a + 23), pal = (u16)rd16(g, a + 24), flip = (u16)rd16(g, a + 26);
    u32 bpp = 4u << siz, tmemBytes = fmt == 2 ? 2048 : 4096;
    s32 y, rows;
    if (flip || (siz != 1 && siz != 2) || ((g->omH >> 20) & 3) != 2 || imageW <= 0 || imageH <= 0)
    {
        g->abort = 1;   // not handled here: the LLE RSP draws it
        return;
    }
    if (frameX < 0) { imageX += -frameX; frameW += frameX; frameX = 0; }
    if (frameY < 0) { imageY += -frameY; frameH += frameY; frameY = 0; }
    if (frameW <= 0 || frameH <= 0) return;
    imageX %= imageW;
    imageY %= imageH;
    for (y = 0; y < frameH; y += rows)
    {
        s32 sy = (imageY + y) % imageH, x, span;
        rows = frameH - y;
        if (rows > imageH - sy) rows = imageH - sy;   // the picture wraps vertically
        for (x = 0; x < frameW; x += span)
        {
            s32 sx = (imageX + x) % imageW, r, band;
            u32 lineBytes;
            span = frameW - x;
            if (span > imageW - sx) span = imageW - sx;   // and horizontally
            lineBytes = ((u32)span * bpp + 63) / 64 * 8;
            band = (s32)(tmemBytes / lineBytes);
            if (band < 1) { g->abort = 1; return; }
            for (r = 0; r < rows; r += band)
            {
                s32 n = rows - r < band ? rows - r : band, ty = sy + r, dx = frameX + x, dy = frameY + y + r;
                u64 w[2];
                u32 line = lineBytes / 8;
                out_rdp1(g, 0xFD000000u | (fmt << 21) | (siz << 19) | (u32)(imageW - 1), ptr);           // SETTIMG
                out_rdp1(g, 0xF5000000u | (fmt << 21) | (siz << 19) | (line << 9), 0x07000000u);        // SETTILE 7
                out_rdp1(g, 0xE6000000u, 0);                                                            // LOADSYNC
                out_rdp1(g, 0xF4000000u | ((u32)sx << 14) | ((u32)ty << 2),
                         0x07000000u | ((u32)(sx + span - 1) << 14) | ((u32)(ty + n - 1) << 2));        // LOADTILE
                out_rdp1(g, 0xE7000000u, 0);                                                            // PIPESYNC
                out_rdp1(g, 0xF5000000u | (fmt << 21) | (siz << 19) | (line << 9), (pal & 15) << 20);  // SETTILE 0
                out_rdp1(g, 0xF2000000u | ((u32)sx << 14) | ((u32)ty << 2),
                         ((u32)(sx + span - 1) << 14) | ((u32)(ty + n - 1) << 2));                      // SETTILESIZE 0
                // TEXRECT in copy mode: inclusive lower-right corner, dsdx = 4.0.
                w[0] = ((u64)(0xE4000000u | ((u32)(dx + span - 1) << 14) | ((u32)(dy + n - 1) << 2)) << 32) |
                       (((u32)dx << 14) | ((u32)dy << 2));
                w[1] = ((u64)(((u32)sx << 21) | ((u32)ty << 5)) << 32) | 0x10000400u;
                out_rdp(g, w, 2);
            }
        }
    }
}

// G_BG_1CYC draws a picture from RDRAM scaled, in 1-cycle mode with the
// game's combiner and blender (Majora's Mask's cutscene and pause
// backgrounds; with the task left to the LLE RSP they ran at 9-15 VI/s).
// uObjScaleBg (gs2dex.h): the uObjBg fields, then scaleW and scaleH (5.10:
// texels per pixel) at 28 and 30. As the microcode does, bands of image lines
// go to TMEM through tile 7 (one more line and column for the bilinear
// filter) and a scaled rectangle draws each band with tile 0. The picture is
// split where it wraps horizontally; a flipped, 4-bit or 32-bit picture, or
// one that wraps vertically, is left to the LLE RSP.
static void bg_1cyc(H64Gfx *g, u32 addr)
{
    u32 a = seg_to_phys(g, addr);
    s32 imageX = (u16)rd16(g, a + 0), imageW = (u16)rd16(g, a + 2) >> 2;   // 10.5, texels
    s32 frameX = rd16(g, a + 4), frameW = (u16)rd16(g, a + 6);             // 10.2
    s32 imageY = (u16)rd16(g, a + 8), imageH = (u16)rd16(g, a + 10) >> 2;
    s32 frameY = rd16(g, a + 12), frameH = (u16)rd16(g, a + 14);
    u32 ptr = seg_to_phys(g, rd32(g, a + 16));
    u32 fmt = rd8(g, a + 22), siz = rd8(g, a + 23), pal = (u16)rd16(g, a + 24), flip = (u16)rd16(g, a + 26);
    s32 scaleW = (u16)rd16(g, a + 28), scaleH = (u16)rd16(g, a + 30);
    u32 bpp = 4u << siz, tmemBytes = fmt == 2 ? 2048 : 4096, cycle = (g->omH >> 20) & 3;
    s32 xa, xw;
    if (flip || (siz != 1 && siz != 2) || cycle > 1 || imageW <= 0 || imageH <= 0 || scaleW <= 0 || scaleH <= 0)
    {
        g->abort = 1;   // not handled here: the LLE RSP draws it
        return;
    }
    // A frame starting above or left of the screen: the picture moves on by
    // as many pixels (10.2 pixels * 5.10 texels a pixel = 10.5 texels * 128).
    if (frameX < 0) { imageX += (-frameX) * scaleW / 128; frameW += frameX; frameX = 0; }
    if (frameY < 0) { imageY += (-frameY) * scaleH / 128; frameH += frameY; frameY = 0; }
    if (frameW <= 0 || frameH <= 0) return;
    imageX %= imageW * 32;
    imageY %= imageH * 32;
    if (imageY + (frameH * scaleH) / 128 > imageH * 32) { g->abort = 1; return; }   // wraps vertically
    for (xa = 0; xa < frameW; xa += xw)
    {
        // A piece of the frame's width: up to where the picture wraps, and
        // narrow enough for two lines (and the bilinear column) in TMEM.
        s32 sa = (imageX + xa * scaleW / 128) % (imageW * 32), texW, ya, yh;
        u32 lineBytes;
        xw = frameW - xa;
        if ((s32)((s64)xw * scaleW / 128) > imageW * 32 - sa)
            xw = (s32)(((s64)(imageW * 32 - sa) * 128 + scaleW - 1) / scaleW);
        xw = (xw + 3) & ~3;   // whole pixels
        if (xw <= 0) xw = 4;
        for (;;)
        {
            texW = ((sa & 31) + xw * scaleW / 128) / 32 + 2;
            if (texW > imageW - (sa >> 5)) texW = imageW - (sa >> 5);
            lineBytes = ((u32)texW * bpp + 63) / 64 * 8;
            if (lineBytes * 2 <= tmemBytes || xw <= 4) break;
            xw = (xw / 2 + 3) & ~3;
        }
        if (lineBytes * 2 > tmemBytes) { g->abort = 1; return; }
        for (ya = 0; ya < frameH; ya = yh)
        {
            s32 ta = imageY + ya * scaleH / 128, ty0 = ta >> 5, fit = (s32)(tmemBytes / lineBytes), tyMax, rows, sx0 = sa >> 5;
            u32 line = lineBytes / 8, xh = (u32)(frameX + xa), yt = (u32)(frameY + ya), xl, yl;
            u64 w[2];
            // Screen rows whose texel rows (and the next, bilinear) are in this band.
            tyMax = ty0 + fit - 2;
            yh = ya + (s32)(((s64)((tyMax + 1) * 32 - ta) * 128) / scaleH);
            yh &= ~3;
            if (yh <= ya) yh = ya + 4;
            if (yh > frameH) yh = frameH;
            rows = ((imageY + (yh - 1) * scaleH / 128) >> 5) - ty0 + 2;
            if (rows > fit) rows = fit;
            if (ty0 + rows > imageH) rows = imageH - ty0;
            if (rows < 1) break;
            xl = xh + (u32)xw;
            yl = yt + (u32)(yh - ya);
            out_rdp1(g, 0xFD000000u | (fmt << 21) | (siz << 19) | (u32)(imageW - 1), ptr);                    // SETTIMG
            out_rdp1(g, 0xF5000000u | (fmt << 21) | (siz << 19) | (line << 9), 0x07000000u);                 // SETTILE 7
            out_rdp1(g, 0xE6000000u, 0);                                                                     // LOADSYNC
            out_rdp1(g, 0xF4000000u | ((u32)sx0 << 14) | ((u32)ty0 << 2),
                     0x07000000u | ((u32)(sx0 + texW - 1) << 14) | ((u32)(ty0 + rows - 1) << 2));            // LOADTILE
            out_rdp1(g, 0xE7000000u, 0);                                                                     // PIPESYNC
            out_rdp1(g, 0xF5000000u | (fmt << 21) | (siz << 19) | (line << 9),
                     ((pal & 15) << 20) | (2u << 18) | (2u << 8));                                           // SETTILE 0, clamped
            out_rdp1(g, 0xF2000000u | ((u32)sx0 << 14) | ((u32)ty0 << 2),
                     ((u32)(sx0 + texW - 1) << 14) | ((u32)(ty0 + rows - 1) << 2));                          // SETTILESIZE 0
            // TEXRECT (1-cycle: exclusive lower-right corner), s/t in 10.5, steps in 5.10.
            w[0] = ((u64)(0xE4000000u | ((xl & 0xFFF) << 12) | (yl & 0xFFF)) << 32) | (((xh & 0xFFF) << 12) | (yt & 0xFFF));
            w[1] = ((u64)((((u32)sa & 0xFFFF) << 16) | ((u32)ta & 0xFFFF)) << 32) |
                   (((u32)scaleW & 0xFFFF) << 16) | ((u32)scaleH & 0xFFFF);
            out_rdp(g, w, 2);
        }
    }
}

static void run_s2dex2(H64Gfx *g, u32 w0, u32 w1)
{
    u32 cmd = w0 >> 24;
    switch (cmd)
    {
    case 0x00: case 0xE0: break;                                   // G_NOOP, G_SPNOOP
    case 0x09: bg_1cyc(g, w1); break;                              // G_BG_1CYC
    case 0x0A: bg_copy(g, w1); break;                              // G_BG_COPY
    case 0x0B: break;                                              // G_OBJ_RENDERMODE (copy mode ignores it)
    case 0xDB:          // G_MOVEWORD
        if (((w0 >> 16) & 0xFF) == 0x06) g->segments[((w0 & 0xFFFF) >> 2) & 0xF] = w1 & 0x00FFFFFF;
        break;
    case 0xDD:          // G_LOAD_UCODE
        if (!load_ucode(g, w1, g->half1, (w0 & 0xFFFF) + 1)) g->abort = 1;
        break;
    case 0xDE: call_dl(g, w1, ((w0 >> 16) & 0xFF) == 0); break;   // G_DL
    case 0xDF: end_dl(g); break;                                   // G_ENDDL
    case 0xE1: g->half1 = w1; break;                               // G_RDPHALF_1
    case 0xE2: case 0xE3:   // G_SETOTHERMODE_L / _H
    {
        u32 len = (w0 & 0xFF) + 1;
        s32 shift = 32 - (s32)((w0 >> 8) & 0xFF) - (s32)len;
        if (shift < 0) shift = 0;
        set_othermode(g, cmd == 0xE3, (u32)shift, len, w1);
        break;
    }
    case 0xF1: break;   // G_RDPHALF_2
    default:
        if (cmd >= 0xC0 && cmd != 0xDA && cmd != 0xDC) rdp_passthrough(g, w0, w1);
        else
        {
            if (!(g->warned & 8)) { H64_INFO("[gfx] S2DEX2 command %02X not handled: task left to the LLE RSP", cmd); g->warned |= 8; }
            g->abort = 1;
        }
        break;
    }
}

// ---------------------------------------------------------------- task
H64Gfx *h64_gfx_create(H64System *sys)
{
    init_tables();
    H64Gfx *g = new H64Gfx;
    int i;
    g->sys = sys;
    g->ucodeCount = g->ucodeNext = 0;
    g->uc = 0;
    memset(g->segments, 0, sizeof(g->segments));
    for (i = 0; i < GFX_STACK_MAX; i++) mat_identity(g->mv[i]);
    mat_identity(g->proj);
    mat_identity(g->combined);
    g->mvi = 0;
    g->stackSize = GFX_STACK_MAX;
    g->combinedValid = 0;
    g->forcedCombined = 0;
    memset(g->vtx, 0, sizeof(g->vtx));
    g->geomRaw = 0;
    g->omH = 0;
    g->omL = 0;
    memset(g->lightCol, 0, sizeof(g->lightCol));
    memset(g->lightDir, 0, sizeof(g->lightDir));
    g->numLights = 0;
    memset(g->lookat, 0, sizeof(g->lookat));
    memset(g->vscale, 0, sizeof(g->vscale));
    memset(g->vtrans, 0, sizeof(g->vtrans));
    g->clipRatio = 1.0f;
    g->texScaleS = g->texScaleT = 1.0f;
    g->texLevel = g->texTile = 0;
    g->texOn = 0;
    g->fogMul = g->fogOff = 0;
    g->half1 = 0;
    g->pci = 0;
    g->halt = g->abort = 0;
    g->fullSync = 0;
    g->commands = 0;
    g->nVerts = g->nTris = 0;
    g->warned = 0;
    g->timgAddr = g->timgWidth = g->timgSize = 0;
    memset(g->cimg, 0, sizeof(g->cimg));
    g->cimgNext = 0;
    g->texFromCimg = 0;
    g->snapshot = 0;
    return g;
}

void h64_gfx_free(H64Gfx *g)
{
    h64_big_free(g->snapshot);
    delete g;
}

static void flush_output(H64Gfx *g)
{
    H64System *sys = g->sys;
    u64 t0 = h64_prof_now(sys);
    H64Renderer *r = sys->renderer;
    size_t i;
    for (i = 0; i < g->ops.size(); i++)
    {
        const GfxOp *op = &g->ops[i];
        if (op->kind == 0)
        {
            if (r) r->rdp(r->user, &g->words[op->index], op->count);
            else h64_rdp_command(sys, &g->words[op->index], op->count);
            sys->dpCommands++;
        }
        else
        {
            const GfxTri *t = &g->tris[op->index];
            if (r) r->triangle(r->user, &t->v[0], &t->v[1], &t->v[2], t->flags, t->tile, t->levels);
            else
            {
                u64 w[22];
                u32 n = h64_rdp_build_triangle(&t->v[0], &t->v[1], &t->v[2], t->flags, t->tile, t->levels, w);
                h64_rdp_command(sys, w, n);
            }
            sys->dpCommands++;
        }
    }
    sys->prof[H64_PROF_RENDER] += h64_prof_now(sys) - t0;
}

int h64_gfx_task_known(H64System *sys, H64Gfx *g)
{
    const u8 *dmem = sys->spMem;
    u32 ucStart = h64_load_be32(dmem + 0xFD0), ucData = h64_load_be32(dmem + 0xFD8);
    u32 ucDataSize = h64_load_be32(dmem + 0xFDC);
    return find_ucode(g, ucStart & 0x7FFFFF, ucData & 0x7FFFFF, ucDataSize)->type != UC_NONE;
}

static int parse_task(H64System *sys, H64Gfx *g, int *fullSync);

// How long the real microcode would keep the RSP busy, in CPU cycles: 130 RCP
// cycles per vertex and 230 per triangle, fitted on the LLE RSP's timing
// (h64test --gfx-cost-log: DK64, SM64, MK64 average 0.5-0.7 M RCP cycles a
// task). Experimental (--gfx-timing): DK64's intro running twice as fast
// with the HLE came from the DP interrupt ignoring DPC_STATUS.FREEZE
// (h64_rdp_hle_full_sync), not from the task time.
u32 h64_gfx_cost(const H64Gfx *g)
{
    return (130u * g->nVerts + 230u * g->nTris) * 3u / 2u;
}

int h64_gfx_measure(H64System *sys, H64Gfx *g, u32 *verts, u32 *tris, u32 *commands)
{
    int fullSync;
    if (!parse_task(sys, g, &fullSync)) return 0;
    *verts = g->nVerts;
    *tris = g->nTris;
    *commands = g->commands;
    g->words.clear();
    g->tris.clear();
    g->ops.clear();
    return 1;
}

int h64_gfx_run_task(H64System *sys, H64Gfx *g, int *fullSync)
{
    if (!parse_task(sys, g, fullSync)) return 0;
    flush_output(g);
    return 1;
}

int h64_gfx_parse_task(H64System *sys, H64Gfx *g, int *fullSync, int *mustSync)
{
    size_t k;
    g->loadRanges.clear();
    g->texFromCimg = 0;
    if (!parse_task(sys, g, fullSync)) return 0;
    // A task that reads a colour image as a texture renders from the snapshot
    // too: the renderer copies the image back into both (h64_gfx_render).
    *mustSync = 0;
    if (!g->snapshot) g->snapshot = (u8 *)h64_big_alloc(H64_RDRAM_SIZE);
    if (!g->snapshot) { *mustSync = 1; return 1; }
    {
        // Ranges sorted and merged first: a display list loads the same
        // texels many times (Conker's cutscenes: thousands of loads a frame).
        u64 t0 = h64_prof_now(sys);
        std::vector<u64> &r = g->rangeSort;
        size_t n = 0;
        r.clear();
        for (k = 0; k + 1 < g->loadRanges.size(); k += 2) r.push_back((u64)g->loadRanges[k] << 32 | g->loadRanges[k + 1]);
        std::sort(r.begin(), r.end());
        while (n < r.size())
        {
            u32 lo = (u32)(r[n] >> 32), hi = (u32)r[n];
            for (n++; n < r.size() && (u32)(r[n] >> 32) <= hi; n++)
                if ((u32)r[n] > hi) hi = (u32)r[n];
            memcpy(g->snapshot + lo, sys->rdram + lo, hi - lo);
            sys->prof[H64_PROF_SNAP_BYTES] += hi - lo;
        }
        sys->prof[H64_PROF_GFX_SNAPSHOT] += h64_prof_now(sys) - t0;
    }
    return 1;
}

void h64_gfx_render(H64System *sys, H64Gfx *g)
{
    H64RdpState *st = h64_rdp_state(sys);
    // Texture loads read the snapshot taken when the display list was parsed.
    // A colour image the GPU drew and the task reads as a texture (OoT's Link
    // on the pause screen, Perfect Dark's cutscene blur) is copied back into
    // the snapshot as well as RDRAM, so the task need not finish rendering
    // before the CPU goes on (PD's cutscenes: the CPU waited ~12 ms a frame).
    st->loadRam = g->snapshot;
    flush_output(g);
    st->loadRam = 0;
}

static int parse_task(H64System *sys, H64Gfx *g, int *fullSync)
{
    const u8 *dmem = sys->spMem;
    u32 ucStart = h64_load_be32(dmem + 0xFD0), ucData = h64_load_be32(dmem + 0xFD8);
    u32 ucDataSize = h64_load_be32(dmem + 0xFDC);
    u32 stack = h64_load_be32(dmem + 0xFE4) >> 6;
    const GfxUcode *u = find_ucode(g, ucStart & 0x7FFFFF, ucData & 0x7FFFFF, ucDataSize);
    if (u->type == UC_NONE)
        return 0;
    g->uc = u;

    // Per-task state, as the microcode starts each task (GLideN64 RSP_ProcessDList).
    g->pc[0] = h64_load_be32(dmem + 0xFF0) & 0x7FFFFF;
    g->pci = 0;
    g->halt = g->abort = 0;
    g->fullSync = 0;
    g->commands = 0;
    g->nVerts = g->nTris = 0;
    g->stackSize = stack == 0 || stack > GFX_STACK_MAX ? GFX_STACK_MAX : (int)stack;
    g->mvi = 0;
    g->combinedValid = 0;
    g->forcedCombined = 0;
    g->geomRaw = 0;
    g->dmaMtx = g->dmaVtx = g->colorBase = 0;
    g->texOffset = g->texShift = g->texCount = 0;
    g->billboard = g->vertexi = 0;
    g->dlCount = -1;
    g->normalBase = 0;
    g->advLighting = 0;   // GLideN64 resets it with each display list
    memset(g->lightCol, 0, sizeof(g->lightCol));
    memset(g->lightDir, 0, sizeof(g->lightDir));
    g->numLights = 0;
    memset(g->lookat, 0, sizeof(g->lookat));
    g->lookat[0][1] = 1.0f;
    g->lookat[1][0] = 1.0f;
    g->words.clear();
    g->tris.clear();
    g->ops.clear();

    while (!g->halt && !g->abort)
    {
        u32 a = g->pc[g->pci];
        u32 w0 = rd32(g, a), w1 = rd32(g, a + 4);
        g->pc[g->pci] = a + 8;
        if (g->uc->type == UC_F3DEX2) run_f3dex2(g, w0, w1);
        else if (g->uc->type == UC_S2DEX2) run_s2dex2(g, w0, w1);
        else run_f3d(g, w0, w1);
        if (g->dlCount > 0 && --g->dlCount == 0)
        {
            // G_DMA_DL: its commands are done (GLideN64 RSP_CheckDLCounter).
            g->dlCount = -1;
            if (g->pci > 0) g->pci--;
        }
        if (++g->commands > GFX_MAX_COMMANDS)
        {
            H64_WARN("[gfx] display list too long (loop?): task left to the LLE RSP");
            g->abort = 1;
        }
    }
    if (g->abort)
        return 0;
    *fullSync = g->fullSync;
    return 1;
}
