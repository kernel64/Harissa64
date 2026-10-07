// Harissa64 V2 - Xenos renderer (see h64_xenos.h).
//
// Combiner inputs, blender modes and texture addressing follow the RDP
// description in the n64brew wiki and ParaLLEl-RDP (as ported in core/rdp);
// the mapping of blender modes onto GPU blending is modelled on GLideN64's
// approach (GPL v2, studied, not copied).
#include "h64_xenos.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>

#include "../../core/common/h64_endian.h"
#include "../../core/common/h64_log.h"
#include "../../core/rdp/h64_rdp.h"
#include "../../core/rdp/h64_rdp_state.h"
#include "../../core/system/h64_system.h"

// EDRAM layout (tiles of 5120 bytes, 80x16 pixels): the 1280x720 back
// buffer uses tiles 0..719, the 960x720 N64 colour target 720..1259 and its
// depth 1260..1799 (2048 in all). 960x720 is the 4:3 picture of the 720p
// output, so frames are shown 1:1.
#define RT_WIDTH 960
#define RT_HEIGHT 720
#define EDRAM_N64_COLOR 720
#define EDRAM_N64_DEPTH 1260
#define FB_SLOTS 8
#define MAX_TEXTURES 1500
#define MAX_TEXTURE_BYTES (40u * 1024u * 1024u)
#define MAX_POOL_BYTES (24u * 1024u * 1024u)
#define BATCH_VERTICES 3000

struct XVtx
{
    float x, y, z, w;
    D3DCOLOR color;
    float u0, v0, u1, v1;
};

struct FbSlot
{
    u32 addr;          // RDRAM colour image
    u32 width, height; // N64 pixels
    u32 bytes;         // bytes per pixel
    IDirect3DTexture9 *tex;   // resolved copy (RT_WIDTH x RT_HEIGHT)
    int valid;
    int gpuDirty;      // drawn by the GPU since the last copy back to RDRAM
    u32 lastUse;
};

struct TexEntry
{
    IDirect3DTexture9 *tex;
    u32 w, h;
};

struct TexBinding
{
    u32 w, h;          // decoded size (texels)
    s32 ox, oy;        // tile texel (before masking) at the decoded texture's origin (a window)
    int clampS, clampT, mirrorS, mirrorT, wrapS, wrapT;
};

struct Xenos
{
    H64Renderer api;
    H64System *sys;
    IDirect3DDevice9 *dev;
    H64RdpState *st;

    IDirect3DSurface9 *backBuffer, *n64Color, *n64Depth;
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *psCopy, *psFill, *psFallback;
    IDirect3DTexture9 *dummy;
    IDirect3DTexture9 *cpuFb[2];
    int cpuFbNext;

    std::map<u64, IDirect3DPixelShader9 *> shaders;
    std::map<u64, TexEntry> textures;
    u32 textureBytes;   // memory held by the texture cache
    // Free textures by size (w << 16 | h), for reuse: CreateTexture is slow on
    // the console, and OoT's backgrounds (S2DEX, LLE) load ~90 strips a frame.
    std::map<u32, std::vector<IDirect3DTexture9 *> > pool;
    u32 poolBytes;
    u64 combineRaw;

    // Batching: triangles with the same state go to one draw.
    std::vector<XVtx> batch;
    int stateDirty;     // an RDP command arrived since the state was last set up
    u32 batchFlags, batchTile;
    TexBinding batchTb0, batchTb1;

    // Texture of each tile until TMEM or the tile changes.
    u32 tmemGen;
    struct { u32 gen; u32 rasterFlags; IDirect3DTexture9 *tex; TexBinding b; } memo[8];

    FbSlot fb[FB_SLOTS];
    int curSlot;        // slot whose image is in the N64 render target (-1: none)
    int edramOwner;     // slot whose colour and depth the N64 EDRAM target holds (-1: none)
    std::vector<u32> copyBuf;
    u32 copyBacks;
    u32 useCounter;
    int debug;
    IDirect3DTexture9 *shown;   // texture shown at the last present
    float shownU, shownV;
    int shownTiled;
    int targetBound;    // the N64 target is bound (not the back buffer)

    std::vector<u32> decodeBuf;
    std::map<u64, int> modeLog;   // xenosdebug=4
    // Texels a rectangle samples on its tile (tile-relative, inclusive), so that
    // only that window is decoded: OoT's backgrounds (S2DEX, LLE) sample a strip
    // of tiles declared up to 1024x1024 (10 million texels decoded a frame).
    struct { int active; u32 tile; s32 s0, s1, t0, t1; } win;
    H64XenosStats stats;
};

static Xenos *X(H64Renderer *r) { return (Xenos *)r; }

// ---------------------------------------------------------------- shaders
static IDirect3DPixelShader9 *compile_ps(IDirect3DDevice9 *dev, const char *src)
{
    LPD3DXBUFFER code = NULL, err = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    HRESULT hr = D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", "ps_3_0", 0, &code, &err, NULL);
    if (FAILED(hr))
    {
        // The error and the source, line by line (the log sink takes one line at a time).
        char line[256];
        const char *p = src;
        H64_ERROR("[xenos] pixel shader compile failed (hr %08X): %s", (u32)hr,
                  err ? (const char *)err->GetBufferPointer() : "no message");
        while (*p)
        {
            size_t n = strcspn(p, "\n");
            if (n > sizeof(line) - 1) n = sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = 0;
            H64_ERROR("[xenos]   | %s", line);
            p += n;
            if (*p == '\n') p++;
        }
        if (err) err->Release();
        return NULL;
    }
    dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &ps);
    code->Release();
    if (err) err->Release();
    return ps;
}

static IDirect3DVertexShader9 *compile_vs(IDirect3DDevice9 *dev, const char *src)
{
    LPD3DXBUFFER code = NULL, err = NULL;
    IDirect3DVertexShader9 *vs = NULL;
    if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", "vs_3_0", 0, &code, &err, NULL)))
    {
        H64_ERROR("[xenos] vertex shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return NULL;
    }
    dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &vs);
    code->Release();
    if (err) err->Release();
    return vs;
}

// Vertex positions arrive in N64 pixels; c0 = (2 / fb width, -2 / fb height).
static const char s_vsSource[] =
    "float4 scale : register(c0);\n"
    "struct VIN { float4 pos : POSITION; float4 col : COLOR0; float4 tc : TEXCOORD0; };\n"
    "struct VOUT { float4 pos : POSITION; float4 col : COLOR0; float4 tc : TEXCOORD0; };\n"
    "VOUT main(VIN i) {\n"
    "  VOUT o; float w = i.pos.w;\n"
    "  o.pos = float4((i.pos.x * scale.x - 1.0) * w, (i.pos.y * scale.y + 1.0) * w, i.pos.z * w, w);\n"
    "  o.col = i.col; o.tc = i.tc; return o; }\n";

static const char s_psCopySource[] =
    "sampler t0 : register(s0);\n"
    "float4 main(float4 col : COLOR0, float4 tc : TEXCOORD0) : COLOR { return tex2D(t0, tc.xy); }\n";

// Used when a combiner shader does not compile: texture times shade.
static const char s_psFallbackSource[] =
    "sampler t0 : register(s0);\n"
    "float4 main(float4 col : COLOR0, float4 tc : TEXCOORD0) : COLOR { return tex2D(t0, tc.xy) * col; }\n";

static const char s_psFillSource[] =
    "float4 fill : register(c0);\n"
    "float4 main(float4 col : COLOR0, float4 tc : TEXCOORD0) : COLOR { return fill; }\n";

// Combiner inputs. Constants: c0 primitive, c1 environment, c2 fog colour,
// c3 blend colour, c4 (prim LOD fraction, K4, K5, LOD fraction), c5 key centre, c6 key scale.
static const char *rgb_a(u32 s)
{
    static const char *t[16] = { "comb.rgb", "t0.rgb", "t1.rgb", "prim.rgb", "shade.rgb", "env.rgb", "1.0", "noise.rgb",
                                 "0.0", "0.0", "0.0", "0.0", "0.0", "0.0", "0.0", "0.0" };
    return t[s & 15];
}
static const char *rgb_b(u32 s)
{
    static const char *t[16] = { "comb.rgb", "t0.rgb", "t1.rgb", "prim.rgb", "shade.rgb", "env.rgb", "keyc.rgb", "misc.yyy",
                                 "0.0", "0.0", "0.0", "0.0", "0.0", "0.0", "0.0", "0.0" };
    return t[s & 15];
}
static const char *rgb_c(u32 s)
{
    static const char *t[32] = { "comb.rgb", "t0.rgb", "t1.rgb", "prim.rgb", "shade.rgb", "env.rgb", "keys.rgb", "comb.aaa",
                                 "t0.aaa", "t1.aaa", "prim.aaa", "shade.aaa", "env.aaa", "misc.www", "misc.xxx", "misc.zzz",
                                 "0.0", "0.0", "0.0", "0.0", "0.0", "0.0", "0.0", "0.0",
                                 "0.0", "0.0", "0.0", "0.0", "0.0", "0.0", "0.0", "0.0" };
    return t[s & 31];
}
static const char *rgb_d(u32 s)
{
    static const char *t[8] = { "comb.rgb", "t0.rgb", "t1.rgb", "prim.rgb", "shade.rgb", "env.rgb", "1.0", "0.0" };
    return t[s & 7];
}
static const char *a_abd(u32 s)
{
    static const char *t[8] = { "comb.a", "t0.a", "t1.a", "prim.a", "shade.a", "env.a", "1.0", "0.0" };
    return t[s & 7];
}
static const char *a_c(u32 s)
{
    static const char *t[8] = { "misc.w", "t0.a", "t1.a", "prim.a", "shade.a", "env.a", "misc.x", "0.0" };
    return t[s & 7];
}

enum { KEY_TWO_CYCLE = 1, KEY_FOG = 2 };

static void combiner_cycle(char *out, size_t len, const H64RdpCombiner *c)
{
    sprintf_s(out, len,
              "  comb = float4(saturate((%s - %s) * %s + %s), saturate((%s - %s) * %s + %s));\n",
              rgb_a(c->rgbMulAdd), rgb_b(c->rgbMulSub), rgb_c(c->rgbMul), rgb_d(c->rgbAdd), a_abd(c->aMulAdd),
              a_abd(c->aMulSub), a_c(c->aMul), a_abd(c->aAdd));
}

static IDirect3DPixelShader9 *combiner_shader(Xenos *x, u32 flags)
{
    u64 key = (x->combineRaw & 0x00FFFFFFFFFFFFFFull) | ((u64)flags << 56);
    std::map<u64, IDirect3DPixelShader9 *>::iterator it = x->shaders.find(key);
    char src[4096], c0[512], c1[512];
    IDirect3DPixelShader9 *ps;
    if (it != x->shaders.end())
        return it->second;
    combiner_cycle(c0, sizeof(c0), &x->st->combiner[0]);
    if (flags & KEY_TWO_CYCLE) combiner_cycle(c1, sizeof(c1), &x->st->combiner[1]);
    else c1[0] = 0;
    if (x->debug == 1) { strcpy(c0, "  comb = float4(1.0, 0.0, 0.0, 1.0);\n"); c1[0] = 0; }
    if (x->debug == 2) { strcpy(c0, "  comb = float4(shade.rgb, 1.0);\n"); c1[0] = 0; }
    if (x->debug == 3) { strcpy(c0, "  comb = float4(t0.rgb, 1.0);\n"); c1[0] = 0; }
    sprintf_s(src, sizeof(src),
              "sampler s0 : register(s0);\n"
              "sampler s1 : register(s1);\n"
              "float4 prim : register(c0);\n"
              "float4 env : register(c1);\n"
              "float4 fogc : register(c2);\n"
              "float4 blendc : register(c3);\n"
              "float4 misc : register(c4);\n"
              "float4 keyc : register(c5);\n"
              "float4 keys : register(c6);\n"
              "float4 main(float4 shade : COLOR0, float4 tc : TEXCOORD0, float2 vpos : VPOS) : COLOR {\n"
              "  float4 t0 = tex2D(s0, tc.xy);\n"
              "  float4 t1 = tex2D(s1, tc.zw);\n"
              "  float4 noise = frac(sin(dot(vpos, float2(12.9898, 78.233))) * 43758.5453);\n"
              "  float4 comb = float4(0.0, 0.0, 0.0, 0.0);\n"
              "%s%s%s"
              "  return comb;\n"
              "}\n",
              c0, c1, (flags & KEY_FOG) ? "  comb.rgb = lerp(comb.rgb, fogc.rgb, shade.a);\n" : "");
    ps = compile_ps(x->dev, src);
    x->shaders[key] = ps;
    x->stats.shaderCompiles++;
    return ps;
}

// ---------------------------------------------------------------- textures
static void retire_all_textures(Xenos *x)
{
    std::map<u64, TexEntry>::iterator it;
    x->dev->SetTexture(0, x->dummy);
    x->dev->SetTexture(1, x->dummy);
    x->dev->BlockUntilIdle();
    // The GPU is idle: the textures can go back to the pool (or be released).
    for (it = x->textures.begin(); it != x->textures.end(); ++it)
    {
        TexEntry *e = &it->second;
        if (!e->tex) continue;
        if (x->poolBytes + e->w * e->h * 4 <= MAX_POOL_BYTES)
        {
            x->pool[(e->w << 16) | e->h].push_back(e->tex);
            x->poolBytes += e->w * e->h * 4;
        }
        else
            e->tex->Release();
    }
    x->textures.clear();
    x->textureBytes = 0;
    x->tmemGen++;   // the memos point at released textures
}

static u64 fnv64(u64 h, const u8 *p, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

static u32 tile_extent(u32 lo, u32 hi)
{
    s32 n = (s32)((hi >> 2) - (lo >> 2)) + 1;
    return n < 1 ? 1 : n > 1024 ? 1024 : (u32)n;
}

// Decoded texture size and addressing for one tile.
static void tile_layout(const H64RdpTile *t, TexBinding *b)
{
    u32 tw = tile_extent(t->slo, t->shi), th = tile_extent(t->tlo, t->thi);
    u32 ms = t->maskS ? 1u << (t->maskS > 10 ? 10 : t->maskS) : 0, mt = t->maskT ? 1u << (t->maskT > 10 ? 10 : t->maskT) : 0;
    b->ox = b->oy = 0;
    b->clampS = (t->flags & TILE_CLAMP_S) || !ms;
    b->clampT = (t->flags & TILE_CLAMP_T) || !mt;
    b->mirrorS = !b->clampS && (t->flags & TILE_MIRROR_S);
    b->mirrorT = !b->clampT && (t->flags & TILE_MIRROR_T);
    b->wrapS = !b->clampS && !b->mirrorS;
    b->wrapT = !b->clampT && !b->mirrorT;
    // Clamped: the whole tile (wrapped inside by the mask, if any). Wrapped or
    // mirrored: one period of the mask.
    b->w = b->clampS ? tw : ms;
    b->h = b->clampT ? th : mt;
    // A clamped tile can be declared far larger than TMEM holds (S2DEX strips:
    // up to 1024 texels a side): past the texels TMEM has, the sampler would
    // read the same words again. Decode at most what the tile can address.
    {
        u32 stride = t->stride ? t->stride : 8;
        u32 rowTexels = t->size == 0 ? stride * 2 : t->size == 1 ? stride : stride / 2;   // 32-bit: split halves
        u32 rows = (t->size == 3 || (t->fmt == 2 && t->size <= 1)) ? 2048 / stride : 4096 / stride;
        if (rowTexels < 1) rowTexels = 1;
        if (rows < 1) rows = 1;
        if (b->clampS && b->w > rowTexels) b->w = rowTexels;
        if (b->clampT && b->h > rows) b->h = rows;
        // Wrapped in T over more lines than TMEM holds: line y reads the same
        // TMEM words as line y mod (TMEM bytes / stride), so one such period is
        // the whole texture (a power of two dividing the mask; at least 2 for
        // the odd-line word swap).
        {
            u32 bytes = (t->size == 3 || (t->fmt == 2 && t->size <= 1)) ? 2048 : 4096;
            if (b->wrapT && stride && bytes % stride == 0)
            {
                u32 p = bytes / stride;
                if (p < 2) p = 2;
                if ((p & (p - 1)) == 0 && b->h > p && b->h % p == 0) b->h = p;
            }
        }
    }
}

static s32 mask_coord(u32 mask, int mirror, s32 v)
{
    if (mask)
    {
        s32 m = 1 << mask;
        if (mirror && (v & m)) v = ~v;
        v &= m - 1;
    }
    return v;
}

static IDirect3DTexture9 *lookup_texture(Xenos *x, u32 tileIndex, TexBinding *b);

// The texture of a tile, from the memo while neither TMEM nor the tile changed.
static IDirect3DTexture9 *get_texture(Xenos *x, u32 tileIndex, TexBinding *b)
{
    u32 t = tileIndex & 7, rf = x->st->rasterFlags & (RS_TLUT | RS_TLUT_TYPE);
    if (x->win.active && x->win.tile == t) return lookup_texture(x, t, b);
    if (x->memo[t].gen == x->tmemGen && x->memo[t].rasterFlags == rf && x->memo[t].tex)
    {
        *b = x->memo[t].b;
        return x->memo[t].tex;
    }
    x->memo[t].tex = lookup_texture(x, t, b);
    x->memo[t].b = *b;
    x->memo[t].gen = x->tmemGen;
    x->memo[t].rasterFlags = rf;
    return x->memo[t].tex;
}

static IDirect3DTexture9 *lookup_texture(Xenos *x, u32 tileIndex, TexBinding *b)
{
    const H64RdpState *st = x->st;
    const H64RdpTile *t = &st->tiles[tileIndex & 7];
    int tlut = (st->rasterFlags & RS_TLUT) != 0, tlutType = (st->rasterFlags & RS_TLUT_TYPE) != 0;
    u64 key = 0xCBF29CE484222325ull;
    u32 rowBytes, rows, i, y;
    std::map<u64, TexEntry>::iterator it;
    TexEntry e;
    D3DLOCKED_RECT lr;
    tile_layout(t, b);
    if (x->win.active && x->win.tile == (tileIndex & 7))
    {
        // Per axis, decode only the texels sampled, when that is at most half
        // the texture. A clamped axis keeps the clamp (window inside [0, w)); a
        // wrapped or mirrored one is decoded through its mask over any range
        // (OoT's backgrounds: 1024x1024 mirrored tiles sampled by narrow strips)
        // and the window is then clamped.
        s32 lo = x->win.s0, hi = x->win.s1;
        if (b->clampS) { if (lo < 0) lo = 0; if (hi > (s32)b->w - 1) hi = (s32)b->w - 1; }
        if (lo <= hi && (u32)(hi - lo + 1) * 2 <= b->w)
        {
            b->ox = lo; b->w = (u32)(hi - lo + 1);
            b->clampS = 1; b->mirrorS = b->wrapS = 0;
        }
        lo = x->win.t0; hi = x->win.t1;
        if (b->clampT) { if (lo < 0) lo = 0; if (hi > (s32)b->h - 1) hi = (s32)b->h - 1; }
        if (lo <= hi && (u32)(hi - lo + 1) * 2 <= b->h)
        {
            b->oy = lo; b->h = (u32)(hi - lo + 1);
            b->clampT = 1; b->mirrorT = b->wrapT = 0;
        }
    }
    // Key: tile parameters, the TMEM rows it covers and the palette.
    key = fnv64(key, (const u8 *)t, sizeof(*t));
    key = fnv64(key, (const u8 *)&b->w, sizeof(b->w));
    key = fnv64(key, (const u8 *)&b->h, sizeof(b->h));
    key = fnv64(key, (const u8 *)&b->ox, sizeof(b->ox));
    key = fnv64(key, (const u8 *)&b->oy, sizeof(b->oy));
    i = (u32)(tlut | tlutType << 1);
    key = fnv64(key, (const u8 *)&i, sizeof(i));
    rowBytes = t->stride ? t->stride : 8;
    rows = (b->ox || b->oy) ? 4096 / rowBytes : b->h + 1;   // a window can reach any TMEM line
    if (rowBytes * rows > 4096) rows = 4096 / rowBytes;
    for (y = 0; y < rows; y++)
    {
        u32 off = (t->offset + y * rowBytes) & 0xFFF, n = rowBytes;
        if (off + n > 4096) n = 4096 - off;
        key = fnv64(key, st->tmem + off, n);
        if (t->size == 3 && t->fmt == 0) key = fnv64(key, st->tmem + ((off | 0x800) & 0xFFF), n);
    }
    if (tlut) key = fnv64(key, st->tmem + 0x800, 0x800);
    it = x->textures.find(key);
    if (it != x->textures.end())
        return it->second.tex;

    if (x->textures.size() >= MAX_TEXTURES || x->textureBytes + b->w * b->h * 4 > MAX_TEXTURE_BYTES)
        retire_all_textures(x);
    e.tex = NULL;
    e.w = b->w;
    e.h = b->h;
    {
        std::map<u32, std::vector<IDirect3DTexture9 *> >::iterator pit = x->pool.find((b->w << 16) | b->h);
        if (pit != x->pool.end() && !pit->second.empty())
        {
            e.tex = pit->second.back();
            pit->second.pop_back();
            x->poolBytes -= b->w * b->h * 4;
        }
        else
        {
            if (FAILED(x->dev->CreateTexture(b->w, b->h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &e.tex, NULL)) || !e.tex)
                return x->dummy;
            x->stats.textureCreates++;
        }
    }
    if (SUCCEEDED(e.tex->LockRect(0, &lr, NULL, 0)))
    {
        // Decode into a cached buffer first: texture memory is write-combined.
        u32 count = b->w * b->h, xx;
        if (x->decodeBuf.size() < count) x->decodeBuf.resize(count);
        for (y = 0; y < b->h; y++)
        {
            s32 ty = mask_coord(t->maskT, (t->flags & TILE_MIRROR_T) != 0, (s32)y + b->oy);
            for (xx = 0; xx < b->w; xx++)
            {
                H64RdpTexel tx;
                s32 sx = mask_coord(t->maskS, (t->flags & TILE_MIRROR_S) != 0, (s32)xx + b->ox);
                h64_rdp_fetch_texel(st, t, (u32)sx, (u32)ty, tlut, tlutType, &tx);
                if (t->fmt == 2 && !tlut) tx.c[3] = tx.c[0];   // CI without TLUT: index as intensity
                x->decodeBuf[y * b->w + xx] = ((u32)(tx.c[3] & 0xFF) << 24) | ((u32)(tx.c[0] & 0xFF) << 16) |
                                             ((u32)(tx.c[1] & 0xFF) << 8) | (u32)(tx.c[2] & 0xFF);
            }
        }
        for (y = 0; y < b->h; y++)
            memcpy((u8 *)lr.pBits + y * lr.Pitch, &x->decodeBuf[y * b->w], b->w * 4);
        e.tex->UnlockRect(0);
    }
    x->textures[key] = e;
    x->textureBytes += e.w * e.h * 4;
    x->stats.textureUploads++;
    x->stats.texelsDecoded += b->w * b->h;
    if (b->w * b->h >= 65536) x->stats.bigCount++;
    if (b->w * b->h > x->stats.bigW * x->stats.bigH)
    {
        x->stats.bigW = b->w; x->stats.bigH = b->h;
        x->stats.bigFmt = t->fmt; x->stats.bigSize = t->size; x->stats.bigStride = t->stride;
        x->stats.bigMaskS = t->maskS; x->stats.bigMaskT = t->maskT; x->stats.bigFlags = t->flags;
        x->stats.bigRect = x->win.active;
    }
    return e.tex;
}

static void bind_texture(Xenos *x, u32 stage, u32 tile, TexBinding *b)
{
    u64 t0 = h64_prof_now(x->sys);
    IDirect3DTexture9 *tex = get_texture(x, tile, b);
    x->stats.tTexture += h64_prof_now(x->sys) - t0;
    DWORD filter = (x->st->rasterFlags & (RS_SAMPLE_QUAD)) && !(x->st->rasterFlags & RS_COPY) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    x->dev->SetTexture(stage, tex ? tex : x->dummy);
    x->dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, b->clampS ? D3DTADDRESS_CLAMP : b->mirrorS ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP);
    x->dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, b->clampT ? D3DTADDRESS_CLAMP : b->mirrorT ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP);
    x->dev->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
    x->dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
    x->dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
}

// Texture coordinate (s10.5 units) to normalised coordinates of the decoded tile.
static float tex_coord(float s, u32 shift, s32 lo, u32 size)
{
    float v = s / 32.0f;
    if (shift > 10) v *= (float)(1 << (16 - shift));
    else if (shift) v /= (float)(1 << shift);
    v -= (float)lo / 4.0f;
    return (v + 0.5f) / (float)size;
}

// ---------------------------------------------------------------- framebuffers
static u32 fb_height(u32 width) { return width <= 320 ? 240 : width * 3 / 4; }

static void bind_n64_target(Xenos *x)
{
    if (x->targetBound) return;
    x->dev->SetRenderTarget(0, x->n64Color);
    x->dev->SetDepthStencilSurface(x->n64Depth);
    x->targetBound = 1;
}

// Draws `tex` (its [0, u1] x [0, v1] part) over the rectangle (x0, y0, w, h)
// of a target of tw x th pixels.
static void draw_textured(Xenos *x, IDirect3DTexture9 *tex, float u1, float v1, float x0, float y0, float w, float h,
                          float tw, float th)
{
    XVtx q[4];
    float c[4];
    int i;
    c[0] = 2.0f / tw; c[1] = -2.0f / th; c[2] = 0; c[3] = 0;
    for (i = 0; i < 4; i++)
    {
        q[i].x = x0 + ((i & 1) ? w : 0.0f);
        q[i].y = y0 + ((i & 2) ? h : 0.0f);
        q[i].z = 0;
        q[i].w = 1;
        q[i].color = 0xFFFFFFFF;
        q[i].u0 = (i & 1) ? u1 : 0.0f;
        q[i].v0 = (i & 2) ? v1 : 0.0f;
        q[i].u1 = q[i].v1 = 0;
    }
    x->dev->SetVertexShaderConstantF(0, c, 1);
    x->dev->SetPixelShader(x->psCopy);
    x->dev->SetTexture(0, tex);
    x->dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    x->dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    x->dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    x->dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    x->dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    x->dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    x->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    x->dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    x->dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    x->dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    x->dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(XVtx));
}

static void draw_fullscreen(Xenos *x, IDirect3DTexture9 *tex, float u1, float v1)
{
    draw_textured(x, tex, u1, v1, 0, 0, (float)RT_WIDTH, (float)RT_HEIGHT, (float)RT_WIDTH, (float)RT_HEIGHT);
}

// The N64 picture on the back buffer: 4:3, centred.
static void draw_display(Xenos *x, IDirect3DTexture9 *tex, float u1, float v1)
{
    D3DSURFACE_DESC d;
    float bw, bh, w, h;
    x->backBuffer->GetDesc(&d);
    bw = (float)d.Width;
    bh = (float)d.Height;
    h = bh;
    w = bh * 4.0f / 3.0f;
    if (w > bw) { w = bw; h = bw * 3.0f / 4.0f; }
    draw_textured(x, tex, u1, v1, (bw - w) / 2, (bh - h) / 2, w, h, bw, bh);
}

static void resolve_current(Xenos *x)
{
    FbSlot *s;
    if (x->curSlot < 0 || !x->targetBound) return;
    s = &x->fb[x->curSlot];
    x->dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, s->tex, NULL, 0, 0, NULL, 0.0f, 0, NULL);
    s->valid = 1;
}

static IDirect3DTexture9 *upload_rdram(Xenos *x, u32 origin, u32 width, int bpp32);

// Makes the N64 target hold the colour image the RDP draws to now.
static void select_framebuffer(Xenos *x)
{
    u32 addr = x->st->colorAddr & 0xFFFFFF, i;
    int best = -1;
    FbSlot *s;
    if (x->curSlot >= 0 && x->fb[x->curSlot].addr == addr && x->targetBound)
        return;
    resolve_current(x);
    bind_n64_target(x);
    x->stats.fbSwitches++;
    for (i = 0; i < FB_SLOTS; i++)
        if (x->fb[i].tex && x->fb[i].addr == addr && x->fb[i].valid) { best = (int)i; break; }
    if (best < 0)
    {
        // A colour image the GPU has not drawn yet: it starts black. (Loading
        // its RDRAM content made OoT, which switches between more images than
        // there were slots, wait for the GPU several times per frame.)
        u32 oldest = 0xFFFFFFFF;
        for (i = 0; i < FB_SLOTS; i++)
            if (x->fb[i].lastUse < oldest) { oldest = x->fb[i].lastUse; best = (int)i; }
        s = &x->fb[best];
        s->addr = addr;
        s->valid = 0;
        x->dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, x->debug == 1 ? 0xFF0000FF : 0xFF000000, 1.0f, 0);
    }
    else if (best != x->edramOwner)
    {
        // The EDRAM target holds another image: restore this one (its depth is lost).
        s = &x->fb[best];
        x->dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        draw_fullscreen(x, s->tex, 1.0f, 1.0f);
    }
    else
        s = &x->fb[best];   // still in EDRAM (presenting draws to the back buffer elsewhere in EDRAM)
    x->edramOwner = best;
    s->gpuDirty = 1;
    s->width = x->st->colorWidth;
    s->height = fb_height(s->width);
    s->bytes = x->st->colorFmt == FB_RGBA8888 ? 4 : 2;
    s->lastUse = ++x->useCounter;
    x->curSlot = best;
}

// Writes a GPU frame back into RDRAM (scaled to the N64 size, in the colour
// image's format), so that the RDP or the CPU can read it.
static void flush_batch(Xenos *x);

static void copy_back(Xenos *x, int slot)
{
    FbSlot *s = &x->fb[slot];
    D3DLOCKED_RECT lr;
    u32 y, xx, w = s->width ? s->width : 320, h = s->height ? s->height : 240;
    u8 *ram = x->sys->rdram;
    flush_batch(x);
    if (slot == x->curSlot && x->targetBound) resolve_current(x);
    if (!s->tex || !s->valid) return;
    x->dev->BlockUntilIdle();
    if (FAILED(s->tex->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return;
    if (x->copyBuf.size() < RT_WIDTH * RT_HEIGHT) x->copyBuf.resize(RT_WIDTH * RT_HEIGHT);
    XGUntileSurface(&x->copyBuf[0], RT_WIDTH * 4, NULL, lr.pBits, RT_WIDTH, RT_HEIGHT, NULL, 4);
    s->tex->UnlockRect(0);
    for (y = 0; y < h; y++)
    {
        const u32 *src = &x->copyBuf[(y * RT_HEIGHT / h) * RT_WIDTH];
        for (xx = 0; xx < w; xx++)
        {
            u32 c = src[xx * RT_WIDTH / w], a = s->addr + (y * w + xx) * s->bytes;
            if (a + s->bytes > H64_RDRAM_SIZE) break;
            if (s->bytes == 4)
                h64_store_be32(ram + a, (c << 8) | 0xFF);
            else
                h64_store_be16(ram + a, (u16)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07C0) | ((c >> 2) & 0x003E) | 1));
        }
    }
    h64_jit_notify_write(x->sys, s->addr, w * h * s->bytes);
    s->gpuDirty = 0;
    x->stats.copyBacks++;
}

// Texture loads from a colour image the GPU drew: copy it back first.
static void check_texture_source(Xenos *x)
{
    u32 a = x->st->texAddr & 0xFFFFFF, i;
    for (i = 0; i < FB_SLOTS; i++)
    {
        FbSlot *s = &x->fb[i];
        if (s->gpuDirty && s->tex && a >= s->addr && a < s->addr + s->width * s->height * s->bytes)
        {
            copy_back(x, (int)i);
            return;
        }
    }
}

// ---------------------------------------------------------------- render states
static void set_scissor(Xenos *x, float sx, float sy)
{
    RECT r;
    r.left = (LONG)((x->st->scissorXlo >> 2) * sx);
    r.top = (LONG)((x->st->scissorYlo >> 2) * sy);
    r.right = (LONG)((x->st->scissorXhi >> 2) * sx);
    r.bottom = (LONG)((x->st->scissorYhi >> 2) * sy);
    if (r.right > RT_WIDTH) r.right = RT_WIDTH;
    if (r.bottom > RT_HEIGHT) r.bottom = RT_HEIGHT;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right <= r.left || r.bottom <= r.top) { r.left = r.top = 0; r.right = r.bottom = 1; }
    x->dev->SetScissorRect(&r);
    x->dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
}

static void set_color_const(Xenos *x, u32 reg, u32 rgba)
{
    float c[4];
    c[0] = (float)(rgba >> 24) / 255.0f;
    c[1] = (float)((rgba >> 16) & 0xFF) / 255.0f;
    c[2] = (float)((rgba >> 8) & 0xFF) / 255.0f;
    c[3] = (float)(rgba & 0xFF) / 255.0f;
    x->dev->SetPixelShaderConstantF(reg, c, 1);
}

// Blender: the last cycle's (P * A + M * B) mapped to GPU blending.
static void set_blend(Xenos *x, int lastCycle)
{
    const H64RdpState *st = x->st;
    const u8 *b = st->blend[lastCycle];
    int force = (st->depthBlendFlags & DB_FORCE_BLEND) != 0;
    // P: 0 in, 1 mem, 2 blend colour, 3 fog; A: 0 combined alpha, 1 fog alpha, 2 shade alpha, 3 zero;
    // M: as P; B: 0 one minus A, 1 memory alpha, 2 one, 3 zero.
    if (force && b[0] == 0 && b[2] == 1 && (b[1] == 0 || b[1] == 2) && b[3] == 0)
    {
        x->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        x->dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        x->dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    else if (force && b[0] == 0 && b[2] == 1 && b[3] == 2)
    {
        x->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        x->dev->SetRenderState(D3DRS_SRCBLEND, b[1] == 3 ? D3DBLEND_ZERO : D3DBLEND_SRCALPHA);
        x->dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    }
    else if (force && b[0] == 1 && b[2] == 0 && b[3] == 0)
    {
        // mem * A + in * (1 - A)
        x->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        x->dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVSRCALPHA);
        x->dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
    }
    else
        x->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void set_depth(Xenos *x, int hasDepth)
{
    const H64RdpState *st = x->st;
    int test = hasDepth && (st->depthBlendFlags & DB_DEPTH_TEST);
    int update = hasDepth && (st->depthBlendFlags & DB_DEPTH_UPDATE);
    x->dev->SetRenderState(D3DRS_ZENABLE, test || update ? TRUE : FALSE);
    x->dev->SetRenderState(D3DRS_ZFUNC, test ? D3DCMP_LESSEQUAL : D3DCMP_ALWAYS);
    x->dev->SetRenderState(D3DRS_ZWRITEENABLE, update && st->zMode != 3 ? TRUE : FALSE);
    if (st->zMode == 3)
    {
        // Decal: the N64 passes a pixel whose depth is within the surface's
        // own slope (dz) of the stored one. A bias of a few depth slopes (the
        // N64 pixel is 3 render-target pixels wide) plus a constant keeps
        // decals (Mario's shadow, OoT's ground overlays) on their surface
        // instead of fighting with it.
        float bias = -0.0001f, slope = -6.0f;
        x->dev->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD *)&bias);
        x->dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD *)&slope);
    }
    else
    {
        x->dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
        x->dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    }
}

static void set_alpha_test(Xenos *x)
{
    const H64RdpState *st = x->st;
    if (st->rasterFlags & RS_ALPHA_TEST)
    {
        u32 ref = st->blendColor & 0xFF;
        x->dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        x->dev->SetRenderState(D3DRS_ALPHAFUNC, ref ? D3DCMP_GREATEREQUAL : D3DCMP_GREATER);
        x->dev->SetRenderState(D3DRS_ALPHAREF, ref);
    }
    else if (st->rasterFlags & RS_CVG_TIMES_ALPHA)
    {
        // Coverage times alpha without blending: texels with alpha below one half drop out.
        x->dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        x->dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        x->dev->SetRenderState(D3DRS_ALPHAREF, 0x80);
    }
    else
        x->dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

// Common setup of a combined (1/2-cycle) primitive. Returns the scales from
// N64 pixels to the render target.
// xenosdebug=4: log each new drawing mode once (combiner, other modes, depth,
// tiles), up to 300 lines, to find what a scene draws with (diagnosis).
static void log_mode(Xenos *x, int hasDepth, u32 tile)
{
    const H64RdpState *st = x->st;
    const H64RdpTile *a = &st->tiles[tile & 7], *b = &st->tiles[(tile + 1) & 7];
    u64 key = 0xCBF29CE484222325ull;
    u32 v[8];
    if (x->modeLog.size() >= 300) return;
    v[0] = (u32)x->combineRaw; v[1] = (u32)(x->combineRaw >> 32); v[2] = st->rasterFlags; v[3] = st->depthBlendFlags;
    v[4] = st->zMode | (hasDepth << 4) | (tile << 8); v[5] = st->blend[0][0] | st->blend[0][1] << 4 | st->blend[0][2] << 8 | st->blend[0][3] << 12 |
                                       st->blend[1][0] << 16 | st->blend[1][1] << 20 | st->blend[1][2] << 24 | st->blend[1][3] << 28;
    v[6] = a->fmt | a->size << 4 | a->maskS << 8 | a->maskT << 12 | a->shiftS << 16 | a->shiftT << 20 | a->flags << 24;
    v[7] = b->fmt | b->size << 4 | b->maskS << 8 | b->maskT << 12 | b->shiftS << 16 | b->shiftT << 20 | b->flags << 24;
    key = fnv64(key, (const u8 *)v, sizeof(v));
    if (x->modeLog.find(key) != x->modeLog.end()) return;
    x->modeLog[key] = 1;
    H64_INFO("[mode] cc %08X%08X rf %08X db %08X z %u depth %d tile %u blend %08X t0 %07X t1 %07X prim %08X env %08X primlod %u",
             v[1], v[0], v[2], v[3], st->zMode, hasDepth, tile, v[5], v[6], v[7], st->primColor, st->envColor, st->primLodFrac);
}

static void setup_combined(Xenos *x, int hasDepth, TexBinding *tb0, TexBinding *tb1, u32 tile)
{
    if (x->debug == 4) log_mode(x, hasDepth, tile);
    const H64RdpState *st = x->st;
    int two = (st->rasterFlags & RS_MULTI_CYCLE) != 0;
    u32 flags = two ? KEY_TWO_CYCLE : 0;
    float c[4], fbw = (float)(st->colorWidth ? st->colorWidth : 320), fbh = (float)fb_height(st->colorWidth);
    const u8 *b0 = st->blend[0];
    IDirect3DPixelShader9 *ps;
    if (two && b0[0] == 3 && b0[1] == 2 && b0[2] == 0 && b0[3] == 0) flags |= KEY_FOG;
    if (!two && b0[0] == 3 && b0[1] == 2 && b0[2] == 0 && b0[3] == 0) flags |= KEY_FOG;
    ps = combiner_shader(x, flags);
    x->dev->SetPixelShader(ps ? ps : x->psFallback);
    set_color_const(x, 0, st->primColor);
    set_color_const(x, 1, st->envColor);
    set_color_const(x, 2, st->fogColor);
    set_color_const(x, 3, st->blendColor);
    c[0] = st->primLodFrac / 255.0f;
    c[1] = (float)st->convert[4] / 255.0f;
    c[2] = (float)st->convert[5] / 255.0f;
    c[3] = 0;
    x->dev->SetPixelShaderConstantF(4, c, 1);
    c[0] = st->keyCenter[0] / 255.0f; c[1] = st->keyCenter[1] / 255.0f; c[2] = st->keyCenter[2] / 255.0f; c[3] = 0;
    x->dev->SetPixelShaderConstantF(5, c, 1);
    c[0] = st->keyScale[0] / 255.0f; c[1] = st->keyScale[1] / 255.0f; c[2] = st->keyScale[2] / 255.0f; c[3] = 0;
    x->dev->SetPixelShaderConstantF(6, c, 1);
    c[0] = 2.0f / fbw; c[1] = -2.0f / fbh; c[2] = 0; c[3] = 0;
    x->dev->SetVertexShaderConstantF(0, c, 1);
    bind_texture(x, 0, tile, tb0);
    bind_texture(x, 1, (tile + 1) & 7, tb1);
    set_blend(x, two ? 1 : 0);
    set_depth(x, hasDepth);
    set_alpha_test(x);
    set_scissor(x, RT_WIDTH / fbw, RT_HEIGHT / fbh);
    x->dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
}

static D3DCOLOR rgba_to_d3d(float r, float g, float b, float a)
{
    u32 R = r < 0 ? 0 : r > 255 ? 255 : (u32)r, G = g < 0 ? 0 : g > 255 ? 255 : (u32)g;
    u32 B = b < 0 ? 0 : b > 255 ? 255 : (u32)b, A = a < 0 ? 0 : a > 255 ? 255 : (u32)a;
    return D3DCOLOR_ARGB(A, R, G, B);
}

// ---------------------------------------------------------------- primitives
static void flush_batch(Xenos *x)
{
    u64 t0;
    if (x->batch.empty()) return;
    t0 = h64_prof_now(x->sys);
    x->dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(x->batch.size() / 3), &x->batch[0], sizeof(XVtx));
    x->stats.tDraw += h64_prof_now(x->sys) - t0;
    x->stats.draws++;
    x->batch.clear();
}

static void xenos_triangle(void *user, const H64RenderVertex *a, const H64RenderVertex *b, const H64RenderVertex *c,
                           u32 flags, u32 tile, u32 levels)
{
    Xenos *x = (Xenos *)user;
    const H64RdpState *st = x->st;
    const H64RenderVertex *v[3];
    const H64RdpTile *t0 = &st->tiles[tile & 7], *t1 = &st->tiles[(tile + 1) & 7];
    XVtx q[3];
    int i, persp = (st->rasterFlags & RS_PERSPECTIVE) != 0;
    (void)levels;
    if (st->rasterFlags & (RS_FILL | RS_COPY)) return;
    if (x->stateDirty || flags != x->batchFlags || tile != x->batchTile || x->batch.size() + 3 > BATCH_VERTICES)
    {
        flush_batch(x);
        select_framebuffer(x);
        setup_combined(x, (flags & H64_TRI_ZBUFFER) != 0, &x->batchTb0, &x->batchTb1, tile);
        x->batchFlags = flags;
        x->batchTile = tile;
        x->stateDirty = 0;
    }
    const TexBinding &tb0 = x->batchTb0, &tb1 = x->batchTb1;
    v[0] = a; v[1] = b; v[2] = c;
    for (i = 0; i < 3; i++)
    {
        float w = persp && v[i]->invw > 0 ? 1.0f / v[i]->invw : 1.0f;
        float z = st->usePrimDepth ? (float)(st->primDepth >> 16) : v[i]->z;
        q[i].x = v[i]->x;
        q[i].y = v[i]->y;
        q[i].z = z / 32767.0f;
        q[i].w = w;
        q[i].color = (flags & H64_TRI_SHADE) ? rgba_to_d3d(v[i]->r, v[i]->g, v[i]->b, v[i]->a) : 0;
        q[i].u0 = tex_coord(v[i]->s, t0->shiftS, t0->slo, tb0.w);
        q[i].v0 = tex_coord(v[i]->t, t0->shiftT, t0->tlo, tb0.h);
        q[i].u1 = tex_coord(v[i]->s, t1->shiftS, t1->slo, tb1.w);
        q[i].v1 = tex_coord(v[i]->t, t1->shiftT, t1->tlo, tb1.h);
    }
    x->batch.push_back(q[0]);
    x->batch.push_back(q[1]);
    x->batch.push_back(q[2]);
    x->stats.triangles++;
}

static void fill_rect(Xenos *x, const u32 *w)
{
    const H64RdpState *st = x->st;
    float fbw = (float)(st->colorWidth ? st->colorWidth : 320), fbh = (float)fb_height(st->colorWidth);
    float sx = RT_WIDTH / fbw, sy = RT_HEIGHT / fbh;
    u32 xh = (w[0] >> 12) & 0xFFF, yh = w[0] & 0xFFF, xl = (w[1] >> 12) & 0xFFF, yl = w[1] & 0xFFF;
    D3DRECT r;
    r.x1 = (LONG)((xl >> 2) * sx);
    r.y1 = (LONG)((yl >> 2) * sy);
    r.x2 = (LONG)(((xh >> 2) + 1) * sx);
    r.y2 = (LONG)(((yh >> 2) + 1) * sy);
    if (r.x2 > RT_WIDTH) r.x2 = RT_WIDTH;
    if (r.y2 > RT_HEIGHT) r.y2 = RT_HEIGHT;
    if (r.x1 >= r.x2 || r.y1 >= r.y2) return;
    x->stats.fills++;
    if ((st->colorAddr & 0xFFFFFF) == (st->depthAddr & 0xFFFFFF))
    {
        // Filling the depth image: a depth clear of the frame being drawn.
        if (x->curSlot >= 0 && x->targetBound) x->dev->Clear(1, &r, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        return;
    }
    select_framebuffer(x);
    {
        u32 c = st->fillColor, argb;
        if (st->colorFmt == FB_RGBA8888)
            argb = ((c & 0xFF) << 24) | (c >> 8);
        else
        {
            u32 p = c >> 16, R = (p >> 11) & 31, G = (p >> 6) & 31, B = (p >> 1) & 31;
            argb = 0xFF000000u | ((R << 3 | R >> 2) << 16) | ((G << 3 | G >> 2) << 8) | (B << 3 | B >> 2);
        }
        x->dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        x->dev->Clear(1, &r, D3DCLEAR_TARGET, x->debug == 1 ? 0xFF00FF00 : argb, 1.0f, 0);
    }
}

static void tex_rect(Xenos *x, const u32 *w, int flip)
{
    const H64RdpState *st = x->st;
    int copy = (st->rasterFlags & RS_COPY) != 0;
    u32 tile = (w[1] >> 24) & 7;
    const H64RdpTile *t0 = &st->tiles[tile], *t1 = &st->tiles[(tile + 1) & 7];
    float xh = ((w[0] >> 12) & 0xFFF) / 4.0f, yh = (w[0] & 0xFFF) / 4.0f;
    float xl = ((w[1] >> 12) & 0xFFF) / 4.0f, yl = (w[1] & 0xFFF) / 4.0f;
    float s = (float)(s16)(w[2] >> 16), t = (float)(s16)w[2];
    float dsdx = (float)(s16)(w[3] >> 16) / 1024.0f, dtdy = (float)(s16)w[3] / 1024.0f;
    float z = st->usePrimDepth ? (float)(st->primDepth >> 16) / 32767.0f : 0.0f;
    TexBinding tb0, tb1;
    XVtx q[4];
    int i;
    if (copy)
    {
        dsdx /= 4.0f;
        xh += 1.0f;
        yh += 1.0f;
    }
    if (xh <= xl || yh <= yl) return;
    select_framebuffer(x);
    x->win.active = 0;
    if (!t0->shiftS && !t0->shiftT)
    {
        // The texels sampled, tile-relative, with a texel of margin (bilinear).
        float spanS = flip ? yh - yl : xh - xl, spanT = flip ? xh - xl : yh - yl;
        float sa = s / 32.0f - 0.5f * dsdx, sb = s / 32.0f + (spanS - 0.5f) * dsdx;
        float ta = t / 32.0f - 0.5f * dtdy, tb = t / 32.0f + (spanT - 0.5f) * dtdy;
        if (sa > sb) { float k = sa; sa = sb; sb = k; }
        if (ta > tb) { float k = ta; ta = tb; tb = k; }
        x->win.s0 = (s32)floorf(sa - t0->slo / 4.0f) - 1;
        x->win.s1 = (s32)floorf(sb - t0->slo / 4.0f) + 1;
        x->win.t0 = (s32)floorf(ta - t0->tlo / 4.0f) - 1;
        x->win.t1 = (s32)floorf(tb - t0->tlo / 4.0f) + 1;
        x->win.tile = tile;
        x->win.active = 1;
    }
    if (copy)
    {
        float c[4];
        float fbw = (float)(st->colorWidth ? st->colorWidth : 320), fbh = (float)fb_height(st->colorWidth);
        x->dev->SetPixelShader(x->psCopy);
        bind_texture(x, 0, tile, &tb0);
        tb1 = tb0;
        c[0] = 2.0f / fbw; c[1] = -2.0f / fbh; c[2] = 0; c[3] = 0;
        x->dev->SetVertexShaderConstantF(0, c, 1);
        x->dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        x->dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        x->dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        if (st->rasterFlags & RS_ALPHA_TEST)
        {
            x->dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            x->dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
            x->dev->SetRenderState(D3DRS_ALPHAREF, 0);
        }
        else
            x->dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        set_scissor(x, RT_WIDTH / fbw, RT_HEIGHT / fbh);
    }
    else
    {
        setup_combined(x, 1, &tb0, &tb1, tile);
        if ((st->rasterFlags & RS_SAMPLE_QUAD) && !flip && !t0->shiftS && !t0->shiftT)
        {
            // Bilinear rectangles that stay inside their tile (MK64's title is
            // 2-line strips of a 320x240 picture, wrapped on a 2-line mask):
            // the N64 samples at whole texels there, so wrapping never shows;
            // at a higher resolution it would blend in the strip's other line.
            float s0 = s / 32.0f, t0v = t / 32.0f;
            float s1 = s0 + (xh - xl - 1.0f) * dsdx, t1v = t0v + (yh - yl - 1.0f) * dtdy;
            if (s0 >= t0->slo / 4.0f && s1 <= t0->shi / 4.0f && dsdx > 0)
                x->dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            if (t0v >= t0->tlo / 4.0f && t1v <= t0->thi / 4.0f && dtdy > 0)
                x->dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
    }
    for (i = 0; i < 4; i++)
    {
        // Texture coordinates half a pixel back: the N64 samples a pixel at its
        // top-left corner, the GPU at the centre of each (smaller) pixel, so the
        // middle of each N64 pixel gets exactly the N64's texel.
        float px = (i & 1) ? xh : xl, py = (i & 2) ? yh : yl;
        float ds = (px - xl - 0.5f) * dsdx, dt = (py - yl - 0.5f) * dtdy;
        float ss, tt;
        if (flip) { ss = s + (py - yl - 0.5f) * dsdx * 32.0f; tt = t + (px - xl - 0.5f) * dtdy * 32.0f; }
        else { ss = s + ds * 32.0f; tt = t + dt * 32.0f; }
        q[i].x = px;
        q[i].y = py;
        q[i].z = z;
        q[i].w = 1.0f;
        q[i].color = 0;
        q[i].u0 = tex_coord(ss, t0->shiftS, (s32)t0->slo + 4 * tb0.ox, tb0.w);
        q[i].v0 = tex_coord(tt, t0->shiftT, (s32)t0->tlo + 4 * tb0.oy, tb0.h);
        q[i].u1 = tex_coord(ss, t1->shiftS, t1->slo, tb1.w);
        q[i].v1 = tex_coord(tt, t1->shiftT, t1->tlo, tb1.h);
    }
    x->win.active = 0;
    x->dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(XVtx));
    x->stats.rects++;
    x->stats.draws++;
}

// A raw RDP triangle (LLE graphics): drawn from its rebuilt vertices.
static void raw_triangle(Xenos *x, const u32 *w, u32 op)
{
    H64RenderVertex v[3];
    h64_rdp_decode_triangle(w, (x->st->rasterFlags & RS_PERSPECTIVE) != 0, v);
    xenos_triangle(x, &v[0], &v[1], &v[2], op & 7, (w[0] >> 16) & 7, ((w[0] >> 19) & 7) + 1);
}

static void xenos_rdp(void *user, const u64 *words, u32 count)
{
    Xenos *x = (Xenos *)user;
    u32 w[44], i, op;
    u64 t0, tStart = h64_prof_now(x->sys);
    for (i = 0; i < count && i < 22; i++)
    {
        w[i * 2] = (u32)(words[i] >> 32);
        w[i * 2 + 1] = (u32)words[i];
    }
    op = (w[0] >> 24) & 0x3F;
    if (op != 0x26 && op != 0x27 && op != 0x28 && op != 0x29 && op != 0x00)
    {
        flush_batch(x);
        x->stateDirty = 1;
        // TMEM, tiles or the TLUT mode change: the tile memos are stale.
        if (op == 0x30 || op == 0x32 || op == 0x33 || op == 0x34 || op == 0x35 || op == 0x2F) x->tmemGen++;
    }
    if (op == 0x3C) x->combineRaw = words[0];
    if (op == 0x33 || op == 0x34) check_texture_source(x);
    // State and TMEM: the software RDP (state only).
    t0 = h64_prof_now(x->sys);
    h64_rdp_command(x->sys, words, count);
    x->stats.tState += h64_prof_now(x->sys) - t0;
    switch (op)
    {
    case 0x24: tex_rect(x, w, 0); break;
    case 0x25: tex_rect(x, w, 1); break;
    case 0x36:
        if (x->st->rasterFlags & RS_FILL) fill_rect(x, w);
        else
        {
            // A combined rectangle: drawn as an untextured rect.
            u32 r[4];
            r[0] = w[0]; r[1] = w[1]; r[2] = 0; r[3] = 0;
            tex_rect(x, r, 0);
        }
        break;
    default:
        if (op >= 0x08 && op <= 0x0F) raw_triangle(x, w, op);
        break;
    }
    x->stats.tRdp += h64_prof_now(x->sys) - tStart;
}

// ---------------------------------------------------------------- presentation
// Decodes an RDRAM colour image into one of the two CPU frame textures.
static IDirect3DTexture9 *upload_rdram(Xenos *x, u32 origin, u32 width, int bpp32)
{
    IDirect3DTexture9 *tex = x->cpuFb[x->cpuFbNext];
    D3DLOCKED_RECT lr;
    u32 h = fb_height(width), y, xx;
    const u8 *ram = x->sys->rdram;
    x->cpuFbNext ^= 1;
    if (!tex || width == 0 || width > RT_WIDTH) return NULL;
    tex->BlockUntilNotBusy();
    if (FAILED(tex->LockRect(0, &lr, NULL, 0))) return NULL;
    for (y = 0; y < h && y < RT_HEIGHT; y++)
    {
        u32 *row = (u32 *)((u8 *)lr.pBits + y * lr.Pitch);
        for (xx = 0; xx < width; xx++)
        {
            u32 a = origin + (y * width + xx) * (bpp32 ? 4 : 2), c;
            if (a + 4 > H64_RDRAM_SIZE) { row[xx] = 0; continue; }
            if (bpp32)
            {
                c = h64_load_be32(ram + a);
                row[xx] = 0xFF000000u | (c >> 8);
            }
            else
            {
                u32 p = h64_load_be16(ram + a), R = (p >> 11) & 31, G = (p >> 6) & 31, B = (p >> 1) & 31;
                row[xx] = 0xFF000000u | ((R << 3 | R >> 2) << 16) | ((G << 3 | G >> 2) << 8) | (B << 3 | B >> 2);
            }
        }
    }
    tex->UnlockRect(0);
    return tex;
}

static void show_rdram_frame(Xenos *x, u32 origin, u32 width, int bpp32)
{
    u32 h = fb_height(width);
    IDirect3DTexture9 *tex = upload_rdram(x, origin, width, bpp32);
    if (!tex) return;
    draw_display(x, tex, (float)width / RT_WIDTH, (float)h / RT_HEIGHT);
    x->shown = tex;
    x->shownTiled = 0;
    x->shownU = (float)width / RT_WIDTH;
    x->shownV = (float)h / RT_HEIGHT;
}

void h64_xenos_present(H64Renderer *r)
{
    Xenos *x = X(r);
    flush_batch(x);
    x->stateDirty = 1;
    const u32 *vi = x->sys->vi.regs;
    u32 origin = vi[1] & 0xFFFFFF, width = vi[2] & 0xFFF, type = vi[0] & 3, i;
    int found = -1;
    if (x->curSlot >= 0 && x->targetBound) resolve_current(x);
    for (i = 0; i < FB_SLOTS; i++)
    {
        const FbSlot *s = &x->fb[i];
        if (s->valid && s->tex && origin >= s->addr && origin < s->addr + s->width * s->height * s->bytes)
        {
            found = (int)i;
            break;
        }
    }
    x->dev->SetRenderTarget(0, x->backBuffer);
    x->dev->SetDepthStencilSurface(NULL);
    x->targetBound = 0;
    x->dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    if (type < 2)
        ;
    else if (found >= 0)
    {
        draw_display(x, x->fb[found].tex, 1.0f, 1.0f);
        x->shown = x->fb[found].tex;
        x->shownU = x->shownV = 1.0f;
        x->shownTiled = 1;
    }
    else
        show_rdram_frame(x, origin, width, type == 3);
    x->dev->Present(NULL, NULL, NULL, NULL);
    x->stats.presents++;
    // The next draw rebinds the N64 target and restores its content.
    x->curSlot = -1;
}

// ---------------------------------------------------------------- creation
static IDirect3DTexture9 *make_dummy(IDirect3DDevice9 *dev)
{
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT lr;
    int y;
    if (FAILED(dev->CreateTexture(4, 4, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL)) || !t) return NULL;
    if (SUCCEEDED(t->LockRect(0, &lr, NULL, 0)))
    {
        for (y = 0; y < 4; y++) memset((u8 *)lr.pBits + y * lr.Pitch, 0xFF, 16);
        t->UnlockRect(0);
    }
    return t;
}

H64Renderer *h64_xenos_create(H64System *sys, IDirect3DDevice9 *dev)
{
    static const D3DVERTEXELEMENT9 decl[] = {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        { 0, 20, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    D3DSURFACE_PARAMETERS sp;
    Xenos *x = new Xenos;
    int i;
    x->api.user = x;
    x->api.rdp = xenos_rdp;
    x->api.triangle = xenos_triangle;
    x->sys = sys;
    x->dev = dev;
    sys->options.rdpStateOnly = 1;
    x->st = h64_rdp_state(sys);
    x->combineRaw = 0;
    x->textureBytes = 0;
    x->poolBytes = 0;
    x->stateDirty = 1;
    x->batchFlags = x->batchTile = 0xFFFFFFFF;
    x->tmemGen = 1;
    memset(x->memo, 0, sizeof(x->memo));
    x->batch.reserve(BATCH_VERTICES);
    x->curSlot = -1;
    x->edramOwner = -1;
    x->copyBacks = 0;
    x->useCounter = 0;
    x->debug = 0;
    x->targetBound = 0;
    x->cpuFbNext = 0;
    x->shown = NULL;
    x->shownU = x->shownV = 0;
    memset(&x->stats, 0, sizeof(x->stats));
    memset(x->fb, 0, sizeof(x->fb));

    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &x->backBuffer);
    memset(&sp, 0, sizeof(sp));
    sp.Base = EDRAM_N64_COLOR;
    sp.HierarchicalZBase = 0xFFFFFFFF;
    dev->CreateRenderTarget(RT_WIDTH, RT_HEIGHT, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &x->n64Color, &sp);
    sp.Base = EDRAM_N64_DEPTH;
    sp.HierarchicalZBase = 0;
    dev->CreateDepthStencilSurface(RT_WIDTH, RT_HEIGHT, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &x->n64Depth, &sp);
    dev->CreateVertexDeclaration(decl, &x->decl);
    x->vs = compile_vs(dev, s_vsSource);
    x->psCopy = compile_ps(dev, s_psCopySource);
    x->psFill = compile_ps(dev, s_psFillSource);
    x->psFallback = compile_ps(dev, s_psFallbackSource);
    x->dummy = make_dummy(dev);
    for (i = 0; i < 2; i++)
        if (FAILED(dev->CreateTexture(RT_WIDTH, RT_HEIGHT, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &x->cpuFb[i], NULL)))
            x->cpuFb[i] = NULL;
    // Frame textures are tiled: Resolve writes the tiled layout, and the GPU
    // reads them back the same way (a linear destination came out scrambled).
    for (i = 0; i < FB_SLOTS; i++)
        if (FAILED(dev->CreateTexture(RT_WIDTH, RT_HEIGHT, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &x->fb[i].tex, NULL)))
            x->fb[i].tex = NULL;
    if (!x->n64Color || !x->n64Depth || !x->decl || !x->vs || !x->psCopy || !x->psFill || !x->psFallback || !x->dummy)
    {
        H64_ERROR("[xenos] initialisation failed (target %p depth %p decl %p vs %p ps %p/%p)", (void *)x->n64Color,
                  (void *)x->n64Depth, (void *)x->decl, (void *)x->vs, (void *)x->psCopy, (void *)x->psFill);
        delete x;
        return NULL;
    }
    dev->SetVertexDeclaration(x->decl);
    dev->SetVertexShader(x->vs);
    dev->SetTexture(0, x->dummy);
    dev->SetTexture(1, x->dummy);
    dev->SetRenderState(D3DRS_HALFPIXELOFFSET, TRUE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    H64_INFO("[xenos] renderer ready (EDRAM target %ux%u)", RT_WIDTH, RT_HEIGHT);
    return &x->api;
}

void h64_xenos_free(H64Renderer *r)
{
    Xenos *x = X(r);
    std::map<u64, IDirect3DPixelShader9 *>::iterator it;
    int i;
    if (!x) return;
    x->dev->SetRenderTarget(0, x->backBuffer);
    x->dev->SetDepthStencilSurface(NULL);
    retire_all_textures(x);
    {
        std::map<u32, std::vector<IDirect3DTexture9 *> >::iterator pit;
        size_t k;
        for (pit = x->pool.begin(); pit != x->pool.end(); ++pit)
            for (k = 0; k < pit->second.size(); k++) pit->second[k]->Release();
        x->pool.clear();
    }
    for (it = x->shaders.begin(); it != x->shaders.end(); ++it)
        if (it->second) it->second->Release();
    for (i = 0; i < FB_SLOTS; i++)
        if (x->fb[i].tex) x->fb[i].tex->Release();
    for (i = 0; i < 2; i++)
        if (x->cpuFb[i]) x->cpuFb[i]->Release();
    if (x->dummy) x->dummy->Release();
    if (x->psCopy) x->psCopy->Release();
    if (x->psFill) x->psFill->Release();
    if (x->psFallback) x->psFallback->Release();
    if (x->vs) x->vs->Release();
    if (x->decl) x->decl->Release();
    if (x->n64Color) x->n64Color->Release();
    if (x->n64Depth) x->n64Depth->Release();
    if (x->backBuffer) x->backBuffer->Release();
    x->sys->options.rdpStateOnly = 0;
    delete x;
}

int h64_xenos_save_frame(H64Renderer *r, const char *path)
{
    Xenos *x = X(r);
    D3DLOCKED_RECT lr;
    u32 w, h, y, xx, rowBytes;
    u8 header[54], *row;
    const u8 *bits;
    u32 pitch;
    u32 *untiled = NULL;
    FILE *f;
    if (!x->shown) return -1;
    w = (u32)(x->shownU * RT_WIDTH);
    h = (u32)(x->shownV * RT_HEIGHT);
    x->dev->BlockUntilIdle();
    if (FAILED(x->shown->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return -1;
    bits = (const u8 *)lr.pBits;
    pitch = lr.Pitch;
    if (x->shownTiled)
    {
        untiled = (u32 *)malloc(RT_WIDTH * RT_HEIGHT * 4);
        if (!untiled) { x->shown->UnlockRect(0); return -1; }
        XGUntileSurface(untiled, RT_WIDTH * 4, NULL, lr.pBits, RT_WIDTH, RT_HEIGHT, NULL, 4);
        bits = (const u8 *)untiled;
        pitch = RT_WIDTH * 4;
    }
    f = fopen(path, "wb");
    if (!f) { free(untiled); x->shown->UnlockRect(0); return -1; }
    rowBytes = (w * 3 + 3) & ~3u;
    memset(header, 0, sizeof(header));
    header[0] = 'B'; header[1] = 'M';
    #define PUT32(o, v) do { header[o] = (u8)(v); header[o + 1] = (u8)((v) >> 8); header[o + 2] = (u8)((v) >> 16); header[o + 3] = (u8)((v) >> 24); } while (0)
    PUT32(2, 54 + rowBytes * h);
    PUT32(10, 54);
    PUT32(14, 40);
    PUT32(18, w);
    PUT32(22, h);
    header[26] = 1;
    header[28] = 24;
    PUT32(34, rowBytes * h);
    #undef PUT32
    fwrite(header, 1, sizeof(header), f);
    row = (u8 *)malloc(rowBytes);
    for (y = 0; y < h; y++)
    {
        // Bottom-up rows; the texture holds 0xAARRGGBB words.
        const u32 *src = (const u32 *)(bits + (h - 1 - y) * pitch);
        memset(row, 0, rowBytes);
        for (xx = 0; xx < w; xx++)
        {
            u32 c = src[xx];
            row[xx * 3] = (u8)c;
            row[xx * 3 + 1] = (u8)(c >> 8);
            row[xx * 3 + 2] = (u8)(c >> 16);
        }
        fwrite(row, 1, rowBytes, f);
    }
    free(row);
    free(untiled);
    fclose(f);
    x->shown->UnlockRect(0);
    return 0;
}

void h64_xenos_set_debug(H64Renderer *r, int mode)
{
    Xenos *x = X(r);
    x->debug = mode;
    x->shaders.clear();   // leaks the compiled shaders: debug only
}

void h64_xenos_stats(H64Renderer *r, H64XenosStats *out, int reset)
{
    Xenos *x = X(r);
    *out = x->stats;
    if (reset) memset(&x->stats, 0, sizeof(x->stats));
}
