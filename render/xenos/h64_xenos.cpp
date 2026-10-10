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
#include <string>
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
#define EDRAM_READBACK 1800   // 640x480 colour (240 tiles): frames shrunk to the N64 size for copy-backs
#define RB_WIDTH 640
#define RB_HEIGHT 480
#define FB_SLOTS 8
#define MAX_TEXTURES 8000   // arena textures (4 KB each at least) are bounded by the arena
#define MAX_TEXTURE_BYTES (40u * 1024u * 1024u)
// Texture arena: small textures get their D3D header (XGSetTextureHeader) over
// this physical memory instead of CreateTexture (~400 us each on the console:
// GoldenEye's gun-barrel intro made ~300 new small textures a frame, 110 ms).
// Used in chunks; re-entering a chunk evicts its textures after a GPU fence.
#define ARENA_BYTES (24u * 1024u * 1024u)
#define ARENA_CHUNK (1024u * 1024u)
#define ARENA_CHUNKS (ARENA_BYTES / ARENA_CHUNK)
#define MAX_POOL_BYTES (24u * 1024u * 1024u)
#define BATCH_VERTICES 3000

// The colour as floats (0..1, clamped by the vertex shader): a D3DCOLOR took
// four float-to-integer conversions a vertex, each a load-hit-store stall on
// the Xbox 360's CPU.
struct XVtx
{
    float x, y, z, w;
    float col[4];
    float u0, v0, u1, v1;
};

struct FbSlot
{
    u32 addr;          // RDRAM colour image
    u32 width, height; // N64 pixels (height guessed from the width)
    u32 drawnH;        // lines the GPU drew into (scissor / fills): what a copy back may write
    u32 drawnAt;       // Xenos.presentCount when last selected for drawing
    u32 bytes;         // bytes per pixel (I4: 1, not copied back)
    int fmt;           // FB_*
    IDirect3DTexture9 *tex;   // resolved copy (RT_WIDTH x RT_HEIGHT)
    // The picture the VI still shows while the GPU draws the next frame into
    // this image (created when first needed): see select_framebuffer.
    IDirect3DTexture9 *front;
    int showFront;     // the VI shows `front` (until it shows another image)
    u32 splitAt;       // presentCount of the last swap of tex and front
    int valid;
    int gpuDirty;      // drawn by the GPU since the last copy back to RDRAM
    u32 lastUse;
};

struct TexEntry
{
    IDirect3DTexture9 *tex;
    u32 w, h;
    int chunk;         // arena chunk (-1: a CreateTexture texture)
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
    u32 presentCount;  // VI presents so far (frame slots' age)
    IDirect3DDevice9 *dev;
    H64RdpState *st;

    IDirect3DSurface9 *backBuffer, *n64Color, *n64Depth;
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *psCopy, *psFill, *psFallback;
    IDirect3DPixelShader9 *psSmooth;   // edge smoothing at display (FXAA)
    int smooth;
    // Graphics settings (h64_xenos_set_options).
    int scale;          // internal resolution: 1 (320x240 for a 320-wide image), 2, 3 (960x720)
    u32 rtW, rtH;       // the part of the N64 target drawn at that scale (viewport)
    float uMax, vMax;   // rtW / RT_WIDTH, rtH / RT_HEIGHT: that part in a frame texture
    int texFilter;      // 0 the N64's 3-point filter, 1 bilinear, 2 nearest
    int sharpen, blur, screen;  // display: sharpening 0-2, blur 0-2, screen effect (H64XenosOptions)
    int aspect;                 // 0 4:3, 1 16:9 widescreen (wider 3D), 2 16:9 stretched
    std::map<int, IDirect3DPixelShader9 *> psDisplay;   // display pass variants, compiled on use
    IDirect3DTexture9 *dummy;
    IDirect3DTexture9 *cpuFb[2];
    int cpuFbNext;

    std::map<u64, IDirect3DPixelShader9 *> shaders;
    std::map<u64, int> shaderT1;   // per combiner shader: whether it samples t1 (unit 1)
    int usesT1;                    // the shader set by the last setup_combined
    std::map<u64, TexEntry> textures;
    u32 textureBytes;   // memory held by the texture cache
    // Free textures by size (w << 16 | h), for reuse: CreateTexture is slow on
    // the console, and OoT's backgrounds (S2DEX, LLE) load ~90 strips a frame.
    std::map<u32, std::vector<IDirect3DTexture9 *> > pool;
    u8 *arena;                              // ARENA_BYTES of physical memory (NULL: CreateTexture only)
    u32 arenaHead;                          // next free byte
    std::vector<u64> arenaKeys[ARENA_CHUNKS];          // cache keys of the textures in each chunk
    std::vector<IDirect3DTexture9 *> arenaHeaders[ARENA_CHUNKS];
    u32 poolBytes;
    u32 retires;        // times the texture cache was emptied (it unbinds both texture units)
    u64 combineRaw;

    // Batching: triangles with the same state go to one draw.
    std::vector<XVtx> batch;
    int stateDirty;     // an RDP command arrived since the state was last set up
    u32 batchFlags, batchTile;
    TexBinding batchTb0, batchTb1;
    float batchA[4], batchB[4];   // u0, v0, u1, v1 = s or t * A + B for the batch's tiles

    // Texture of each tile until TMEM or the tile changes.
    u32 tmemGen;
    // Texels loaded from an image the GPU drew (see fb_load / fb_rect_source):
    // rectangles that only show them sample that image directly.
    u32 tmemDataGen;   // TMEM loads so far
    struct
    {
        int valid, slot, block;
        u32 dataGen, slotAddr, slotUse;
        u32 tmemOff, stride;      // the load tile's TMEM offset and line stride (bytes)
        u32 startPix;             // LoadBlock: first texel's index in the image (y * width + x)
        u32 texels;               // LoadBlock: texels loaded
        u32 sl, tl, cols, rows;   // LoadTile: first texel and size of the area loaded
        u32 dxt;
    } fbLoad;
    IDirect3DTexture9 *fbTex;   // set: bind_texture puts this image on unit 0 instead of decoding the tile
    float fbTexels[2];          // its N64 texels across [0, 1] (3-point filter sizes)
    struct { u32 gen; u32 rasterFlags; IDirect3DTexture9 *tex; TexBinding b; } memo[8];

    FbSlot fb[FB_SLOTS];
    int curSlot;        // slot whose image is in the N64 render target (-1: none)
    int edramOwner;     // slot whose colour the N64 EDRAM target holds (-1: none)
    // The depth image (RDRAM address) the EDRAM depth holds (0xFFFFFFFF:
    // unknown). As on the N64, where the Z buffer is an RDRAM image, the
    // depth stays when the game draws into another colour image; it is
    // cleared only when a primitive uses another depth image. Clearing it at
    // each change of colour image let the characters inside Conker's pub
    // show through the closed door (it draws small images mid-frame).
    u32 edramDepthAddr;
    std::vector<u32> copyBuf;
    IDirect3DSurface9 *rbSurf;     // EDRAM target at EDRAM_READBACK
    IDirect3DTexture9 *rbTex;      // its resolve, in CPU-cached memory (D3DUSAGE_CPU_CACHED_MEMORY)
    u32 copyBacks;
    u32 useCounter;
    int debug;
    u32 debugFrom;     // xenosdebug=8: the first of the two presents logged (xenosdebug=8000+N: present N)
    IDirect3DTexture9 *shown;   // texture shown at the last present
    int shownSlot;              // frame slot the VI showed at the last present (-1: none)
    float shownU, shownV;
    int shownTiled;
    void (*overlay)(void *user, IDirect3DDevice9 *dev);   // front-end messages, drawn before each present
    void *overlayUser;
    int drewFrame;      // the RDP has drawn a colour image (until then RDRAM frames are not shown)
    u32 blankPresents;  // presents left black waiting for that
    int targetBound;    // the N64 target is bound (not the back buffer)

    std::vector<u32> decodeBuf;
    std::map<u64, int> modeLog;   // xenosdebug=4
    int deferNotify;              // running on the graphics worker (h64_xenos_set_worker)
    u32 pendingLo, pendingHi;     // RDRAM written meanwhile (copy-backs), for the recompiler
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
// The RDP interpolates shade linearly in screen space, the GPU perspective-
// correctly: the shade also goes out multiplied by w with w beside it, and the
// combiner divides, which gives the screen-linear value (texture coordinates
// stay perspective-correct, as the RDP's are). Fog in the shade alpha on large
// ground triangles (OoT's Kokiri paths) depends on it.
// Depth: the RDP clamps Z per pixel, the GPU clips a triangle at z = 0 and 1.
// The position's z is clamped here (nothing is clipped) and the real z goes
// along (lw.y / lw.x, screen-linear); triangles with a vertex out of range get
// a combiner variant (KEY_DEPTH) that writes the clamped per-pixel depth, with
// the decal bias of set_depth (c7) applied there.
static const char s_vsSource[] =
    "float4 scale : register(c0);\n"
    "struct VIN { float4 pos : POSITION; float4 col : COLOR0; float4 tc : TEXCOORD0; };\n"
    "struct VOUT { float4 pos : POSITION; float4 col : COLOR0; float4 tc : TEXCOORD0; float4 lcol : TEXCOORD1; float4 lw : TEXCOORD2; };\n"
    "VOUT main(VIN i) {\n"
    "  VOUT o; float w = i.pos.w;\n"
    "  o.pos = float4((i.pos.x * scale.x - 1.0) * w, (i.pos.y * scale.y + 1.0) * w, 0.0, w);\n"
    "  o.pos.z = saturate(i.pos.z) * w;\n"
    "  float4 c = saturate(i.col);\n"
    "  o.col = c; o.tc = i.tc; o.lcol = c * w; o.lw = float4(w, i.pos.z * w, 0.0, 0.0); return o; }\n";

static const char s_psCopySource[] =
    "sampler t0 : register(s0);\n"
    "float4 main(float4 col : COLOR0, float4 tc : TEXCOORD0) : COLOR { return tex2D(t0, tc.xy); }\n";

// Edge smoothing at display time, in place of the VI's anti-aliasing filter
// (all three games run the VI in "AA and resample" mode, which blends the
// edges of polygons with the background using the coverage the RDP stored;
// Xenos has no coverage). This is FXAA 2 (Timothy Lottes, public domain, as
// in NVIDIA's FXAA_PC_CONSOLE path): luma-based edge direction, then a blend
// of 2 or 4 taps along the edge, kept only when it stays within the local
// luma range. c0 = (1/width, 1/height) of the frame texture.
static const char s_psSmoothSource[] =
    "sampler t0 : register(s0);\n"
    "float4 rcp : register(c0);\n"
    "float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float4 main(float4 col : COLOR0, float4 tc : TEXCOORD0) : COLOR {\n"
    "  float2 uv = tc.xy;\n"
    "  float3 rgbM = tex2D(t0, uv).rgb;\n"
    "  float lNW = luma(tex2D(t0, uv + float2(-0.5, -0.5) * rcp.xy).rgb);\n"
    "  float lNE = luma(tex2D(t0, uv + float2( 0.5, -0.5) * rcp.xy).rgb);\n"
    "  float lSW = luma(tex2D(t0, uv + float2(-0.5,  0.5) * rcp.xy).rgb);\n"
    "  float lSE = luma(tex2D(t0, uv + float2( 0.5,  0.5) * rcp.xy).rgb);\n"
    "  float lM = luma(rgbM);\n"
    "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
    "  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "  float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "  float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "  float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
    "  dir = clamp(dir * rcpMin, -8.0, 8.0) * rcp.xy;\n"
    "  float3 a = 0.5 * (tex2D(t0, uv + dir * (1.0 / 3.0 - 0.5)).rgb + tex2D(t0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  float3 b = a * 0.5 + 0.25 * (tex2D(t0, uv - dir * 0.5).rgb + tex2D(t0, uv + dir * 0.5).rgb);\n"
    "  float lB = luma(b);\n"
    "  return float4((lB < lMin || lB > lMax) ? a : b, 1.0);\n"
    "}\n";

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

enum { KEY_TWO_CYCLE = 1, KEY_FOG = 2, KEY_DEPTH = 4, KEY_3POINT = 8 };
#define TRI_ZOUT 0x100   // batch flag beside H64_TRI_*: a vertex Z outside the RDP range

// In the second cycle the RDP's TEXEL0 input is texel 1 and TEXEL1 is the next
// pixel's texel 0 (pipelining; ParaLLEl-RDP's shading follows it): t0 and t1
// swap there. OoT's Kokiri paths take their alpha from "TEXEL1" in cycle 2,
// i.e. from the I4 mask in tile 0: with t1 they came out opaque.
static void combiner_cycle(char *out, size_t len, const H64RdpCombiner *c, int second)
{
    char *p;
    sprintf_s(out, len,
              "  comb = float4(float3(saturate((%s - %s) * %s + %s)), saturate((%s - %s) * %s + %s));\n",
              rgb_a(c->rgbMulAdd), rgb_b(c->rgbMulSub), rgb_c(c->rgbMul), rgb_d(c->rgbAdd), a_abd(c->aMulAdd),
              a_abd(c->aMulSub), a_c(c->aMul), a_abd(c->aAdd));
    if (!second) return;
    for (p = out; *p; p++)
        if (p[0] == 't' && (p[1] == '0' || p[1] == '1') && p[2] == '.')
        {
            p[1] = p[1] == '0' ? '1' : '0';
            p += 2;
        }
}

// The RDP's bilinear filter is a 3-point one: of the 2x2 texels around the
// sample it blends the 3 of the triangle the sample falls in (the diagonal
// from the lower-left to the upper-right texel splits the square). Sampled
// with point filtering; tsize = (width, height, 1/width, 1/height).
static const char s_ps3PointSource[] =
    "float4 tex3p(sampler s, float2 uv, float4 tsize) {\n"
    "  float2 p = uv * tsize.xy - 0.5;\n"
    "  float2 f = frac(p);\n"
    "  float2 b = (floor(p) + 0.5) * tsize.zw;\n"
    "  float4 c00 = tex2D(s, b);\n"
    "  float4 c10 = tex2D(s, b + float2(tsize.z, 0.0));\n"
    "  float4 c01 = tex2D(s, b + float2(0.0, tsize.w));\n"
    "  float4 c11 = tex2D(s, b + tsize.zw);\n"
    "  return (f.x + f.y < 1.0) ? c00 + f.x * (c10 - c00) + f.y * (c01 - c00)\n"
    "                           : c11 + (1.0 - f.x) * (c01 - c11) + (1.0 - f.y) * (c10 - c11);\n"
    "}\n";

static IDirect3DPixelShader9 *combiner_shader(Xenos *x, u32 flags)
{
    u64 key = (x->combineRaw & 0x00FFFFFFFFFFFFFFull) | ((u64)flags << 56);
    std::map<u64, IDirect3DPixelShader9 *>::iterator it = x->shaders.find(key);
    char src[4096], c0[512], c1[512];
    IDirect3DPixelShader9 *ps;
    if (it != x->shaders.end())
    {
        x->usesT1 = x->shaderT1[key];
        return it->second;
    }
    combiner_cycle(c0, sizeof(c0), &x->st->combiner[0], 0);
    if (flags & KEY_TWO_CYCLE) combiner_cycle(c1, sizeof(c1), &x->st->combiner[1], 1);
    else c1[0] = 0;
    x->usesT1 = strstr(c0, "t1.") || strstr(c1, "t1.");
    x->shaderT1[key] = x->usesT1;
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
              "float4 zbias : register(c7);\n"
              "float4 tsize0 : register(c8);\n"
              "float4 tsize1 : register(c9);\n"
              "%s"
              "struct PSOUT { float4 c : COLOR; %s };\n"
              "PSOUT main(float4 col : COLOR0, float4 tc : TEXCOORD0, float4 lcol : TEXCOORD1, float4 lw : TEXCOORD2, float2 vpos : VPOS) {\n"
              "  PSOUT o;\n"
              "  float4 shade = saturate(lcol / lw.x);\n"
              "  float4 t0 = %s;\n"
              "  float4 t1 = %s;\n"
              "  float4 noise = frac(sin(dot(vpos, float2(12.9898, 78.233))) * 43758.5453);\n"
              "  float4 comb = float4(0.0, 0.0, 0.0, 0.0);\n"
              "%s%s%s"
              "  o.c = comb;\n"
              "%s"
              "  return o;\n"
              "}\n",
              (flags & KEY_3POINT) ? s_ps3PointSource : "",
              (flags & KEY_DEPTH) ? "float d : DEPTH;" : "",
              (flags & KEY_3POINT) ? "tex3p(s0, tc.xy, tsize0)" : "tex2D(s0, tc.xy)",
              (flags & KEY_3POINT) ? "tex3p(s1, tc.zw, tsize1)" : "tex2D(s1, tc.zw)",
              c0, c1, (flags & KEY_FOG) ? "  comb.rgb = lerp(comb.rgb, fogc.rgb, shade.a);\n" : "",
              // Xenos computes ddx/ddy with a texture unit: a shader whose
              // combiner reads no texture had no sampler left and failed to
              // compile (X3602, Banjo-Kazooie); t0 * zbias.w (always 0) keeps s0.
              (flags & KEY_DEPTH) ? "  float z = lw.y / lw.x;\n"
                                    "  o.d = saturate(z + zbias.x + zbias.y * max(abs(ddx(z)), abs(ddy(z))));\n"
                                    "  o.c += t0 * zbias.w;\n" : "");
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
        if (!e->tex || e->chunk >= 0) continue;   // arena headers: freed below
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
    {
        u32 c, k;
        for (c = 0; c < ARENA_CHUNKS; c++)
        {
            for (k = 0; k < x->arenaHeaders[c].size(); k++) delete x->arenaHeaders[c][k];
            x->arenaHeaders[c].clear();
            x->arenaKeys[c].clear();
        }
        x->arenaHead = 0;
    }
    x->tmemGen++;   // the memos point at released textures
    x->retires++;
}

static u64 fnv64(u64 h, const u8 *p, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

// Hash of TMEM bytes for the texture cache key: 8 bytes at a time in four
// independent lanes (the multiplies overlap on the in-order Xenon), instead of
// FNV byte by byte (up to 4 KB per lookup: 3-6 ms a frame in OoT).
static u64 mem_hash(u64 h, const u8 *p, u32 n)
{
    u64 a = h ^ 0x9E3779B97F4A7C15ull, b = h + 0xC2B2AE3D27D4EB4Full, c = h ^ 0x165667B19E3779F9ull, d = h - 0x85EBCA77C2B2AE63ull;
    u32 i = 0;
    for (; i + 32 <= n; i += 32)
    {
        u64 w0, w1, w2, w3;
        memcpy(&w0, p + i, 8); memcpy(&w1, p + i + 8, 8); memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
        a = (a ^ w0) * 0x100000001B3ull; b = (b ^ w1) * 0x100000001B3ull;
        c = (c ^ w2) * 0x100000001B3ull; d = (d ^ w3) * 0x100000001B3ull;
    }
    for (; i + 8 <= n; i += 8)
    {
        u64 w;
        memcpy(&w, p + i, 8);
        a = (a ^ w) * 0x100000001B3ull;
    }
    for (; i < n; i++) b = (b ^ p[i]) * 0x100000001B3ull;
    h = a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ n;
    h ^= h >> 31;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
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

// Makes room for `size` bytes at arenaHead (moving to the next chunk when it
// does not fit), evicting what an earlier lap left in a chunk entered.
static int arena_alloc(Xenos *x, u32 size, u32 *offset)
{
    u32 c;
    if (!x->arena || size > ARENA_CHUNK) return 0;
    if (x->arenaHead / ARENA_CHUNK != (x->arenaHead + size - 1) / ARENA_CHUNK || x->arenaHead % ARENA_CHUNK == 0)
    {
        u32 next = x->arenaHead % ARENA_CHUNK == 0 ? x->arenaHead : (x->arenaHead / ARENA_CHUNK + 1) * ARENA_CHUNK;
        if (next >= ARENA_BYTES) next = 0;
        x->arenaHead = next;
        c = next / ARENA_CHUNK;
        if (!x->arenaKeys[c].empty())
        {
            // The GPU may still read this chunk's textures: unbind, wait for
            // everything issued so far, then drop them.
            u32 k;
            x->dev->SetTexture(0, x->dummy);
            x->dev->SetTexture(1, x->dummy);
            x->dev->BlockOnFence(x->dev->InsertFence());
            for (k = 0; k < x->arenaKeys[c].size(); k++)
            {
                std::map<u64, TexEntry>::iterator it = x->textures.find(x->arenaKeys[c][k]);
                if (it != x->textures.end() && it->second.chunk == (int)c) x->textures.erase(it);
            }
            for (k = 0; k < x->arenaHeaders[c].size(); k++) delete x->arenaHeaders[c][k];
            x->arenaKeys[c].clear();
            x->arenaHeaders[c].clear();
            x->dev->InvalidateGpuCache(x->arena + (size_t)c * ARENA_CHUNK, ARENA_CHUNK, 0);
            x->tmemGen++;      // memos may point at the dropped textures
            x->retires++;      // both units now hold the dummy: rebind (setup_combined)
            x->stats.arenaEvictions++;
        }
    }
    *offset = x->arenaHead;
    x->arenaHead += (size + 4095) & ~4095u;
    return 1;
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
    // Key: what the decode depends on (not the tile's position: the same texels
    // loaded for another place would be decoded again; GoldenEye's intro made
    // ~300 small new textures a frame), the TMEM rows it covers and the palette.
    {
        u32 k[5];
        k[0] = t->offset;
        k[1] = t->stride;
        k[2] = (u32)t->fmt | (u32)t->size << 8 | (u32)t->palette << 16;
        k[3] = (u32)t->maskS | (u32)t->maskT << 8 | (u32)t->flags << 16;
        k[4] = 0;
        key = fnv64(key, (const u8 *)k, sizeof(k));
    }
    key = fnv64(key, (const u8 *)&b->w, sizeof(b->w));
    key = fnv64(key, (const u8 *)&b->h, sizeof(b->h));
    key = fnv64(key, (const u8 *)&b->ox, sizeof(b->ox));
    key = fnv64(key, (const u8 *)&b->oy, sizeof(b->oy));
    i = (u32)(tlut | tlutType << 1);
    key = fnv64(key, (const u8 *)&i, sizeof(i));
    rowBytes = t->stride ? t->stride : 8;
    {
        // The TMEM rows the decode reads: [oy, oy + h] (+1: bilinear), or with
        // a T mask the masked rows from 0. Hashing all of TMEM for every
        // windowed rectangle cost GoldenEye ~30 ms per frame.
        u32 firstRow = 0;
        u64 h0 = h64_prof_now(x->sys);
        if (t->maskT) rows = 1u << t->maskT;
        else { firstRow = b->oy; rows = b->h + 1; }
        if (rowBytes * rows > 4096) { rows = 4096 / rowBytes; firstRow = 0; }
        {
            // The rows are contiguous in TMEM (wrapping at 4 KB): one or two ranges.
            u32 off = (t->offset + firstRow * rowBytes) & 0xFFF, n = rows * rowBytes, first = n;
            if (off + first > 4096) first = 4096 - off;
            key = mem_hash(key, st->tmem + off, first);
            if (first < n) key = mem_hash(key, st->tmem, n - first);
            if (t->size == 3 && t->fmt == 0)
            {
                // 32-bit textures: the other half of each texel is 2 KB further.
                for (y = firstRow; y < firstRow + rows; y++)
                {
                    u32 o = (t->offset + y * rowBytes) & 0xFFF, m = rowBytes;
                    if (o + m > 4096) m = 4096 - o;
                    key = mem_hash(key, st->tmem + ((o | 0x800) & 0xFFF), m);
                }
            }
        }
        x->stats.tHash += h64_prof_now(x->sys) - h0;
    }
    if (tlut)
    {
        // Only the palette the texture can use: 16 entries (4-bit, palette
        // number from the tile) or 256; each entry is written 4 times (8 bytes).
        if (t->size == 0) key = mem_hash(key, st->tmem + 0x800 + ((t->palette & 15) << 7), 128);
        else key = mem_hash(key, st->tmem + 0x800, 0x800);
    }
    it = x->textures.find(key);
    if (it != x->textures.end())
        return it->second.tex;
    if (x->debug == 8 && x->presentCount >= x->debugFrom && x->presentCount < x->debugFrom + 2)
    {
        // xenosdebug=8: every texture decoded during two frames (cache misses).
        u32 th = 0, k;
        for (k = 0; k < 4096; k += 4) th = th * 31 + h64_load_be32(st->tmem + k);
        H64_INFO("[xtex] miss at present %u: tile %u fmt %u size %u tlut %d/%d pal %u off %03X stride %u mask %u/%u flags %X "
                 "%ux%u at %d,%d win %d | last image %06X | key %08X%08X tmem %08X", x->presentCount, tileIndex & 7, t->fmt, t->size,
                 tlut, tlutType, t->palette, t->offset, t->stride, t->maskS, t->maskT, t->flags, b->w, b->h, b->ox, b->oy,
                 x->win.active && x->win.tile == (tileIndex & 7), st->texAddr, (u32)(key >> 32), (u32)key, th);
    }

    if (x->textures.size() >= MAX_TEXTURES || x->textureBytes + b->w * b->h * 4 > MAX_TEXTURE_BYTES)
    {
        retire_all_textures(x);
        x->stats.retires++;
    }
    u64 d0 = h64_prof_now(x->sys);
    e.tex = NULL;
    e.w = b->w;
    e.h = b->h;
    e.chunk = -1;
    {
        // Decode into a cached buffer first: texture memory is write-combined.
        u32 count = b->w * b->h, xx;
        u64 f0 = h64_prof_now(x->sys);
        if (x->decodeBuf.size() < count) x->decodeBuf.resize(count);
        for (y = 0; y < b->h; y++)
        {
            s32 ty = mask_coord(t->maskT, (t->flags & TILE_MIRROR_T) != 0, (s32)y + b->oy);
            if (h64_rdp_fetch_row_argb(st, t, (u32)b->ox, (u32)ty, b->w, tlut, &x->decodeBuf[y * b->w])) continue;
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
        x->stats.tFill += h64_prof_now(x->sys) - f0;
    }
    {
        // Linear textures: rows 256-byte aligned, data 4 KB aligned.
        u32 pitch = (b->w * 4 + 255) & ~255u, offset;
        if (arena_alloc(x, pitch * b->h, &offset))
        {
            u64 c0 = h64_prof_now(x->sys);
            IDirect3DTexture9 *t9 = new D3DTexture;
            u8 *dst = x->arena + offset;
            XGSetTextureHeader(b->w, b->h, 1, 0, D3DFMT_LIN_A8R8G8B8, (D3DPOOL)0, 0, XGHEADER_CONTIGUOUS_MIP_OFFSET, pitch, t9, NULL, NULL);
            XGOffsetResourceAddress(t9, dst);
            for (y = 0; y < b->h; y++) memcpy(dst + y * pitch, &x->decodeBuf[y * b->w], b->w * 4);
            __sync();   // out of the write-combining buffers before the GPU reads it
            e.tex = t9;
            e.chunk = (int)(offset / ARENA_CHUNK);
            x->arenaKeys[e.chunk].push_back(key);
            x->arenaHeaders[e.chunk].push_back(t9);
            x->stats.arenaTextures++;
            x->stats.tCreate += h64_prof_now(x->sys) - c0;
        }
    }
    if (!e.tex)
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
            u64 c0 = h64_prof_now(x->sys);
            if (FAILED(x->dev->CreateTexture(b->w, b->h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &e.tex, NULL)) || !e.tex)
                return x->dummy;
            x->stats.textureCreates++;
            x->stats.tCreate += h64_prof_now(x->sys) - c0;
        }
        {
            u64 l0 = h64_prof_now(x->sys);
            if (SUCCEEDED(e.tex->LockRect(0, &lr, NULL, 0)))
            {
                for (y = 0; y < b->h; y++)
                    memcpy((u8 *)lr.pBits + y * lr.Pitch, &x->decodeBuf[y * b->w], b->w * 4);
                e.tex->UnlockRect(0);
            }
            x->stats.tLock += h64_prof_now(x->sys) - l0;
        }
        x->textureBytes += e.w * e.h * 4;
    }
    x->textures[key] = e;
    x->stats.tDecode += h64_prof_now(x->sys) - d0;
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
    IDirect3DTexture9 *tex;
    if (stage == 0 && x->fbTex)
    {
        // A frame the GPU drew, sampled directly (fb_rect_source).
        memset(b, 0, sizeof(*b));
        b->w = (u32)x->fbTexels[0];
        b->h = (u32)x->fbTexels[1];
        b->clampS = b->clampT = 1;
        tex = x->fbTex;
    }
    else
        tex = get_texture(x, tile, b);
    x->stats.tTexture += h64_prof_now(x->sys) - t0;
    DWORD filter = (x->st->rasterFlags & (RS_SAMPLE_QUAD)) && !(x->st->rasterFlags & RS_COPY) && x->texFilter == 1 ? D3DTEXF_LINEAR
                                                                                                             : D3DTEXF_POINT;
    float sz[4];
    sz[0] = (float)(b->w ? b->w : 1);
    sz[1] = (float)(b->h ? b->h : 1);
    sz[2] = 1.0f / sz[0];
    sz[3] = 1.0f / sz[1];
    if (stage == 0 && x->fbTex)
    {
        sz[0] = x->fbTexels[0];
        sz[1] = x->fbTexels[1];
        sz[2] = 1.0f / sz[0];
        sz[3] = 1.0f / sz[1];
    }
    x->dev->SetPixelShaderConstantF(8 + stage, sz, 1);   // tsize0/tsize1 (3-point filter)
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

// tex_coord(s, shift, lo, size) = s * A + B.
static void tc_affine(u32 shift, s32 lo, u32 size, float *a, float *b)
{
    float scale = shift > 10 ? (float)(1 << (16 - shift)) : shift ? 1.0f / (float)(1 << shift) : 1.0f;
    float sz = (float)(size ? size : 1);
    *a = scale / 32.0f / sz;
    *b = (0.5f - (float)lo / 4.0f) / sz;
}

// ---------------------------------------------------------------- framebuffers
static u32 fb_height(u32 width) { return width <= 320 ? 240 : width * 3 / 4; }

static void bind_n64_target(Xenos *x)
{
    D3DVIEWPORT9 vp;
    if (x->targetBound) return;
    x->dev->SetRenderTarget(0, x->n64Color);
    x->dev->SetDepthStencilSurface(x->n64Depth);
    // The internal resolution: the frame is drawn in the top-left rtW x rtH of the target.
    vp.X = 0; vp.Y = 0; vp.Width = x->rtW; vp.Height = x->rtH; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    x->dev->SetViewport(&vp);
    x->targetBound = 1;
}

// Draws `tex` (its [0, u1] x [0, v1] part) over the rectangle (x0, y0, w, h)
// of a target of tw x th pixels.
static void draw_textured_ps(Xenos *x, IDirect3DPixelShader9 *ps, IDirect3DTexture9 *tex, float u1, float v1, float x0,
                             float y0, float w, float h, float tw, float th);

static void draw_textured(Xenos *x, IDirect3DTexture9 *tex, float u1, float v1, float x0, float y0, float w, float h,
                          float tw, float th)
{
    draw_textured_ps(x, x->psCopy, tex, u1, v1, x0, y0, w, h, tw, th);
}

static void draw_textured_uv(Xenos *x, IDirect3DPixelShader9 *ps, IDirect3DTexture9 *tex, float u0, float v0, float u1,
                             float v1, float x0, float y0, float w, float h, float tw, float th);

static void draw_textured_ps(Xenos *x, IDirect3DPixelShader9 *ps, IDirect3DTexture9 *tex, float u1, float v1, float x0,
                             float y0, float w, float h, float tw, float th)
{
    draw_textured_uv(x, ps, tex, 0.0f, 0.0f, u1, v1, x0, y0, w, h, tw, th);
}

// Draws the [u0, u1] x [v0, v1] part of `tex` over the rectangle (x0, y0, w, h)
// of a target of tw x th pixels.
static void draw_textured_uv(Xenos *x, IDirect3DPixelShader9 *ps, IDirect3DTexture9 *tex, float u0, float v0, float u1,
                             float v1, float x0, float y0, float w, float h, float tw, float th)
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
        q[i].col[0] = q[i].col[1] = q[i].col[2] = q[i].col[3] = 1.0f;
        q[i].u0 = (i & 1) ? u1 : u0;
        q[i].v0 = (i & 2) ? v1 : v0;
        q[i].u1 = q[i].v1 = 0;
    }
    x->dev->SetVertexShaderConstantF(0, c, 1);
    x->dev->SetPixelShader(ps ? ps : x->psCopy);
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

// The display pass: the N64 picture on the back buffer, with the options of
// the menu's Graphics page, after what modern emulators offer (RetroArch's
// shader presets, GLideN64, ParaLLEl). One shader per combination, built on
// first use. Constants: c0 = (1/w, 1/h, w, h) of the frame texture; c1 = (u, v
// of the drawn part, N64 pixels across, N64 lines); c2 = (scanlines, 0, 0, 0).
// - Smoothing: FXAA 2 (as s_psSmoothSource).
// - Blur: a 3x3 Gaussian (1-2-1) over N64 pixels, 0.75 (soft) or 1.5
//   (strong) pixels apart: the soft picture of a composite cable.
// - Sharpening: contrast-adaptive, after the idea of AMD FidelityFX CAS (MIT):
//   the 4 neighbours subtracted with a weight that shrinks where the local
//   contrast is already high, so edges do not ring.
// - Screen: 1 scanlines, 5 light scanlines; 2 CRT and 3 curved CRT (without smoothing, blur or
//   sharpening), after Timothy Lottes's CRT
//   shader (public domain): each pixel is lit by the two nearest N64 lines
//   with a Gaussian beam that widens on bright colours, a horizontally soft
//   beam and a glow. No phosphor mask: the 1280x720 picture reaches the TV
//   scaled by the console (x1.5 for 1080p), and any mask finer than a few
//   pixels beat into coloured vertical bands (moire: the user saw them; 1-pixel
//   R/G/B columns and a smooth triad per N64 pixel alike, measured on the
//   console's front buffer), as modern CRT shaders advise at non-integer scales; 3 adds the tube's curvature, a
//   vignette and rounded corners; 4 LCD: dark gaps between the N64 pixels.
// Sampling is kept inside the drawn part (a column of the next frame bled in
// at the right edge).
static IDirect3DPixelShader9 *display_shader(Xenos *x)
{
    // The CRTs sample the picture itself, 6 times per pixel: smoothing,
    // sharpening or blur in each sample ran the Xenos microcode compiler out
    // of registers (20-40 s, then a failure: the console seemed frozen), and
    // the beam and mask would undo them anyway.
    int crt = x->screen == 2 || x->screen == 3;
    int smooth = crt ? 0 : x->smooth, sharpen = crt ? 0 : x->sharpen, blur = crt ? 0 : x->blur;
    int key = (smooth ? 1 : 0) | (sharpen << 1) | (blur << 3) | (x->screen << 5);
    std::map<int, IDirect3DPixelShader9 *>::iterator it = x->psDisplay.find(key);
    char head[160];
    std::string src;
    if (it != x->psDisplay.end()) return it->second;
    sprintf_s(head, sizeof(head), "#define SMOOTH %d\n#define SHARPEN %d\n#define BLUR %d\n#define SCREEN %d\n", smooth ? 1 : 0,
              sharpen, blur, x->screen);
    src = head;
    src +=
        "sampler t0 : register(s0);\n"
        "float4 rcp : register(c0);\n"
        "float4 prm : register(c1);\n"
        "float4 prm2 : register(c2);\n"
        "float4 prm3 : register(c3);\n"
        "float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
        "float3 tap(float2 uv) { return tex2D(t0, clamp(uv, prm3.xy + 0.5 * rcp.xy, prm.xy - 0.5 * rcp.xy)).rgb; }\n"
        "float3 base(float2 uv) {\n"
        "#if SMOOTH\n"
        "  uv = clamp(uv, prm3.xy + 0.5 * rcp.xy, prm.xy - 0.5 * rcp.xy);\n"
        "  float3 rgbM = tex2D(t0, uv).rgb;\n"
        "  float lNW = luma(tap(uv + float2(-0.5, -0.5) * rcp.xy));\n"
        "  float lNE = luma(tap(uv + float2( 0.5, -0.5) * rcp.xy));\n"
        "  float lSW = luma(tap(uv + float2(-0.5,  0.5) * rcp.xy));\n"
        "  float lSE = luma(tap(uv + float2( 0.5,  0.5) * rcp.xy));\n"
        "  float lM = luma(rgbM);\n"
        "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
        "  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
        "  float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
        "  float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);\n"
        "  float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
        "  dir = clamp(dir * rcpMin, -8.0, 8.0) * rcp.xy;\n"
        "  float3 a = 0.5 * (tap(uv + dir * (1.0 / 3.0 - 0.5)) + tap(uv + dir * (2.0 / 3.0 - 0.5)));\n"
        "  float3 b = a * 0.5 + 0.25 * (tap(uv - dir * 0.5) + tap(uv + dir * 0.5));\n"
        "  float lB = luma(b);\n"
        "  return (lB < lMin || lB > lMax) ? a : b;\n"
        "#else\n"
        "  return tap(uv);\n"
        "#endif\n"
        "}\n"
        "float3 picture(float2 uv) {\n"
        "  float3 c = base(uv);\n"
        "#if BLUR\n"
        "  float2 d = (prm.xy - prm3.xy) / prm.zw * (BLUR == 1 ? 0.75 : 1.5);\n"
        "  c = (4.0 * c + 2.0 * (tap(uv + float2(d.x, 0.0)) + tap(uv - float2(d.x, 0.0)) + tap(uv + float2(0.0, d.y)) + tap(uv - float2(0.0, d.y)))\n"
        "       + tap(uv + d) + tap(uv - d) + tap(uv + float2(d.x, -d.y)) + tap(uv + float2(-d.x, d.y))) / 16.0;\n"
        "#elif SHARPEN\n"
        "  float3 n = tap(uv - float2(0.0, rcp.y)), so = tap(uv + float2(0.0, rcp.y));\n"
        "  float3 w = tap(uv - float2(rcp.x, 0.0)), e = tap(uv + float2(rcp.x, 0.0));\n"
        "  float3 mn = min(c, min(min(n, so), min(w, e))), mx = max(c, max(max(n, so), max(w, e)));\n"
        "  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 0.0001)));\n"
        "  float3 wt = amp * (SHARPEN == 1 ? -0.125 : -0.2);\n"
        "  c = saturate((c + wt * (n + so + w + e)) / (1.0 + 4.0 * wt));\n"
        "#endif\n"
        "  return c;\n"
        "}\n"
        "float4 main(float4 col : COLOR0, float4 tc : TEXCOORD0, float2 vpos : VPOS) : COLOR {\n"
        "  float2 n = (tc.xy - prm3.xy) / (prm.xy - prm3.xy);\n"
        "#if SCREEN == 3\n"
        "  float2 cc = n * 2.0 - 1.0;\n"
        "  cc *= 1.0 + float2(0.045, 0.06) * (cc.yx * cc.yx);\n"
        "  n = cc * 0.5 + 0.5;\n"
        "  float2 edge = saturate((0.5 - abs(n - 0.5)) * float2(80.0, 60.0));\n"
        "  if (edge.x * edge.y <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);\n"
        "#endif\n"
        "  float2 uv = prm3.xy + n * (prm.xy - prm3.xy);\n"
        "#if SCREEN == 2 || SCREEN == 3\n"
        "  float lines = prm2.x;\n"
        "  float y = n.y * lines - 0.5;\n"
        "  float y0 = floor(y), f = y - y0;\n"
        "  float2 px = float2((prm.x - prm3.x) / prm.z * 0.5, 0.0);\n"
        "  float2 uA = float2(uv.x, prm3.y + (y0 + 0.5) / lines * (prm.y - prm3.y));\n"
        "  float2 uB = float2(uv.x, prm3.y + (y0 + 1.5) / lines * (prm.y - prm3.y));\n"
        "  float3 cA = 0.25 * (tap(uA - px) + tap(uA + px)) + 0.5 * tap(uA);\n"
        "  float3 cB = 0.25 * (tap(uB - px) + tap(uB + px)) + 0.5 * tap(uB);\n"
        "  float kA = lerp(18.0, 6.0, luma(cA)), kB = lerp(18.0, 6.0, luma(cB));\n"
        "  float3 c = cA * exp(-f * f * kA) + cB * exp(-(1.0 - f) * (1.0 - f) * kB);\n"
        "  float3 glow = 0.25 * (tap(uv + float2(4.0, 0.0) * rcp.xy) + tap(uv - float2(4.0, 0.0) * rcp.xy) + tap(uv + float2(0.0, 4.0) * rcp.xy) + tap(uv - float2(0.0, 4.0) * rcp.xy));\n"
        "  c = c * 1.3 + glow * 0.12;\n"
        "#if SCREEN == 3\n"
        "  float2 v = n * (1.0 - n);\n"
        "  c *= pow(saturate(v.x * v.y * 24.0), 0.18) * edge.x * edge.y;\n"
        "#endif\n"
        "#elif SCREEN == 1\n"
        "  float3 c = picture(uv);\n"
        "  float d = frac(n.y * prm2.x) - 0.5;\n"
        "  c *= exp(-d * d * lerp(10.0, 4.0, luma(c))) * 1.3;\n"
        "#elif SCREEN == 5\n"
        "  float3 c = picture(uv);\n"
        "  float d = frac(n.y * prm2.x) - 0.5;\n"
        "  c *= lerp(0.78, 1.08, exp(-d * d * 8.0));\n"
        "#elif SCREEN == 4\n"
        "  float3 c = picture(uv);\n"
        "  float2 g = frac(n * prm.zw);\n"
        "  float2 gap = smoothstep(0.0, 0.18, g) * smoothstep(1.0, 0.82, g);\n"
        "  c *= lerp(0.55, 1.08, gap.x * gap.y);\n"
        "#else\n"
        "  float3 c = picture(uv);\n"
        "#endif\n"
        "  return float4(saturate(c), 1.0);\n"
        "}\n";
    x->psDisplay[key] = compile_ps(x->dev, src.c_str());
    return x->psDisplay[key];
}

// What the VI shows, as ParaLLEl-RDP's VI decodes its registers (as
// core/vi/h64_vi.cpp): the part of the output (640 x 240 NTSC, 288 PAL; [0, 1]
// fractions, dx/dy) covered by the picture (H_START/H_END, V_START/V_END) and
// the framebuffer area shown there (X/Y_START and X/Y_SCALE, in pixels from
// the origin). Showing the whole colour image instead put DK64's letterboxed
// cutscenes (fewer VI lines, centred by V_START) at the top of the screen.
struct ViGeom { float dx0, dy0, dx1, dy1, sx0, sy0, sx1, sy1; int lines, serrate; };

static int vi_geometry(const u32 *v, ViGeom *g)
{
    s32 vStart = (s32)((v[10] >> 16) & 0x3FF), vEnd = (s32)(v[10] & 0x3FF), vSync = (s32)(v[6] & 0x3FF);
    s32 yStart = (s32)((v[13] >> 16) & 0xFFF), yAdd = (s32)(v[13] & 0xFFF);
    s32 xStart = (s32)((v[12] >> 16) & 0xFFF), xAdd = (s32)(v[12] & 0xFFF);
    int isPal = vSync > 525 + 25;
    s32 vEndMax = isPal ? ((44 + 576) | 1) : ((34 + 480) | 1), vOffset = isPal ? 44 : 34, hOffset = isPal ? 128 : 108;
    s32 vRes, hStart, hEnd, outLines = isPal ? 288 : 240;
    if (vEnd > vEndMax) vEnd = vEndMax;
    if (vStart > vEndMax) vStart = vEndMax;
    vRes = (vEnd - vStart) >> 1;
    vStart = (vStart - vOffset) / 2;
    if (vStart < 0) { yStart -= yAdd * vStart; vStart = 0; }
    if (vRes > outLines - vStart) vRes = outLines - vStart;
    hStart = (s32)((v[9] >> 16) & 0x3FF) - hOffset;
    hEnd = (s32)(v[9] & 0x3FF) - hOffset;
    if (hStart < 0) { xStart -= xAdd * hStart; hStart = 0; }
    if (hEnd > 640) hEnd = 640;
    if (hEnd - hStart <= 0 || hStart >= 640 || vRes <= 0 || !xAdd || !yAdd) return 0;
    g->dx0 = hStart / 640.0f;
    g->dx1 = hEnd / 640.0f;
    g->dy0 = (float)vStart / outLines;
    g->dy1 = (float)(vStart + vRes) / outLines;
    g->sx0 = xStart / 1024.0f;
    g->sx1 = g->sx0 + (float)(hEnd - hStart) * xAdd / 1024.0f;
    g->sy0 = yStart / 1024.0f;
    g->sy1 = g->sy0 + (float)vRes * yAdd / 1024.0f;
    g->lines = vRes;
    g->serrate = (v[0] & 0x40) != 0;
    return 1;
}

// The N64 picture on the back buffer: 4:3 and centred, or the whole width
// (16:9 widescreen, whose 3D was drawn wider, or stretched). `g`: what the VI
// shows (NULL: the whole picture); ox/oy: the origin's offset in the image (N64
// pixels); u/v per N64 pixel and line of `tex`; `width`: N64 pixels across.
static void draw_display(Xenos *x, IDirect3DTexture9 *tex, const ViGeom *g, float ox, float oy, float uPer, float vPer,
                         u32 width, u32 lines)
{
    D3DSURFACE_DESC d;
    float bw, bh, w, h, c[4], u0, v0, u1, v1, px0, py0, pw, ph;
    IDirect3DPixelShader9 *ps;
    x->backBuffer->GetDesc(&d);
    bw = (float)d.Width;
    bh = (float)d.Height;
    h = bh;
    w = x->aspect ? bw : bh * 4.0f / 3.0f;
    if (w > bw) { w = bw; h = bw * 3.0f / 4.0f; }
    if (!lines) lines = 240;
    if (!width) width = 320;
    if (g)
    {
        u0 = (g->sx0 + ox) * uPer; u1 = (g->sx1 + ox) * uPer;
        v0 = (g->sy0 + oy) * vPer; v1 = (g->sy1 + oy) * vPer;
        px0 = (bw - w) / 2 + g->dx0 * w; pw = (g->dx1 - g->dx0) * w;
        py0 = (bh - h) / 2 + g->dy0 * h; ph = (g->dy1 - g->dy0) * h;
        lines = (u32)g->lines;
    }
    else
    {
        u0 = v0 = 0.0f;
        u1 = width * uPer; v1 = lines * vPer;
        px0 = (bw - w) / 2; pw = w;
        py0 = (bh - h) / 2; ph = h;
    }
    ps = display_shader(x);
    if (!ps) ps = x->smooth && x->psSmooth ? x->psSmooth : x->psCopy;
    c[0] = 1.0f / RT_WIDTH; c[1] = 1.0f / RT_HEIGHT; c[2] = (float)RT_WIDTH; c[3] = (float)RT_HEIGHT;
    x->dev->SetPixelShaderConstantF(0, c, 1);
    c[0] = u1; c[1] = v1; c[2] = (float)width; c[3] = (float)lines;
    x->dev->SetPixelShaderConstantF(1, c, 1);
    c[0] = (float)(lines > 300 ? lines / 2 : lines); c[1] = c[2] = c[3] = 0;
    x->dev->SetPixelShaderConstantF(2, c, 1);
    c[0] = u0; c[1] = v0; c[2] = c[3] = 0;
    x->dev->SetPixelShaderConstantF(3, c, 1);
    draw_textured_uv(x, ps, tex, u0, v0, u1, v1, px0, py0, pw, ph, bw, bh);
}

// 16:9 widescreen: N64 x coordinates of the 3D and of the small 2D elements
// move 3/4 of the way towards the centre, so the picture shown across 16:9
// keeps their proportions and shows more of the scene at the sides (what the
// game projects beyond the 4:3 screen, within the microcode's clip ratio; as
// GLideN64's widescreen hack, objects the game itself culls stay missing).
// Full-width fills, backgrounds and scissors stay full width.
static float ws_x(const Xenos *x, float px, float fbw)
{
    return x->aspect == 1 ? fbw * 0.5f + (px - fbw * 0.5f) * 0.75f : px;
}

static void resolve_current(Xenos *x)
{
    FbSlot *s;
    if (x->curSlot < 0 || !x->targetBound) return;
    s = &x->fb[x->curSlot];
    x->dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, s->tex, NULL, 0, 0, NULL, 0.0f, 0, NULL);
    s->valid = 1;
    x->drewFrame = 1;
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
        s->drawnH = 0;
        x->dev->Clear(0, NULL, D3DCLEAR_TARGET, x->debug == 1 ? 0xFF0000FF : 0xFF000000, 1.0f, 0);
    }
    else if (best != x->edramOwner)
    {
        // The EDRAM target holds another image: restore this one's colour
        // (the depth is the depth image's: kept, see edramDepthAddr).
        s = &x->fb[best];
        draw_fullscreen(x, s->tex, 1.0f, 1.0f);
    }
    else
        s = &x->fb[best];   // still in EDRAM (presenting draws to the back buffer elsewhere in EDRAM)
    x->edramOwner = best;
    // The game starts the next frame in the image the VI still shows (DK64:
    // it swaps buffers when that frame's display list is done, and the real
    // RDP takes long enough for the VI to have moved on). Our GPU draws the
    // whole frame at once: the VI would show it a frame early, then the other
    // buffer's older frame (pictures back and forth: a "3D TV" shake). The
    // shown picture moves to `front` and the VI keeps showing it until it
    // shows another image; drawing goes on in a fresh texture. Once per
    // present (an image selected several times in a frame keeps one copy).
    if (best == x->shownSlot && s->valid && s->splitAt != x->presentCount + 1)
    {
        if (!s->front &&
            FAILED(x->dev->CreateTexture(RT_WIDTH, RT_HEIGHT, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s->front, NULL)))
            s->front = NULL;
        if (s->front)
        {
            IDirect3DTexture9 *t = s->tex;
            s->tex = s->front;
            s->front = t;
            s->showFront = 1;
            s->splitAt = x->presentCount + 1;
        }
    }
    s->gpuDirty = 1;
    s->drawnAt = x->presentCount;
    s->width = x->st->colorWidth;
    s->height = fb_height(s->width);
    s->fmt = x->st->colorFmt;
    s->bytes = s->fmt == FB_RGBA8888 ? 4 : (s->fmt == FB_RGBA5551 || s->fmt == FB_IA88) ? 2 : 1;
    s->lastUse = ++x->useCounter;
    x->curSlot = best;
}

// Writes a GPU frame back into RDRAM (scaled to the N64 size, in the colour
// image's format), so that the RDP or the CPU can read it.
static void flush_batch(Xenos *x);

// One copied-back pixel (c = 0x00RRGGBB) in the colour image's format. 8-bit
// images took two bytes a pixel before, writing past them (Conker froze).
static void store_pixel(u8 *p, int fmt, u32 c)
{
    u32 i = (((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)) / 3;
    if (fmt == FB_RGBA8888) h64_store_be32(p, (c << 8) | 0xFF);
    else if (fmt == FB_RGBA5551) h64_store_be16(p, (u16)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07C0) | ((c >> 2) & 0x003E) | 1));
    else if (fmt == FB_IA88) h64_store_be16(p, (u16)((i << 8) | 0xFF));
    else *p = (u8)i;   // I8
}

static void copy_back(Xenos *x, int slot)
{
    FbSlot *s = &x->fb[slot];
    D3DLOCKED_RECT lr;
    u32 y, xx, w = s->width ? s->width : 320, h = s->height ? s->height : 240, hc = h;
    u8 *ram = x->sys->rdram;
    // Texture loads may read a snapshot of RDRAM (graphics HLE on the worker): the copy goes there too.
    u8 *snap = (u8 *)x->st->loadRam;
    // Only the lines drawn (hc): an offscreen image (Conker's small render
    // targets) is not as tall as the guess h, which still sets the scale.
    if (s->drawnH && s->drawnH < h) hc = s->drawnH;
    if (s->fmt == FB_I4) { s->gpuDirty = 0; return; }   // 4-bit colour images: not copied back
    flush_batch(x);
    if (slot == x->curSlot && x->targetBound) resolve_current(x);
    if (!s->tex || !s->valid) return;
    if (x->rbSurf && x->rbTex && w <= RB_WIDTH && h <= RB_HEIGHT)
    {
        // Shrink the frame to the N64 size on the GPU (one texel per N64 pixel,
        // sampled at its centre), resolve that into CPU-cached memory and read
        // it: 9 times less data than the whole 960x720 frame, and cached reads
        // instead of write-combined ones (Paper Mario's file-select transition
        // copied 7 frames back per frame: 6 FPS).
        u32 wa = (w + 31) & ~31u, ha = (h + 7) & ~7u, i;
        D3DRECT r;
        RECT ur;
        x->dev->SetRenderTarget(0, x->rbSurf);
        x->dev->SetDepthStencilSurface(NULL);
        x->targetBound = 0;     // the N64 target is bound again (its EDRAM content kept) at the next draw
        x->stateDirty = 1;
        draw_textured(x, s->tex, x->uMax, x->vMax, 0.0f, 0.0f, (float)w, (float)h, (float)RB_WIDTH, (float)RB_HEIGHT);
        r.x1 = 0; r.y1 = 0; r.x2 = (LONG)wa; r.y2 = (LONG)ha;
        x->dev->Resolve(D3DRESOLVE_RENDERTARGET0, &r, x->rbTex, NULL, 0, 0, NULL, 0.0f, 0, NULL);
        x->dev->BlockUntilIdle();
        if (FAILED(x->rbTex->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return;
        // The GPU wrote behind the CPU caches: drop any stale line first.
        for (i = 0; i < RB_WIDTH * RB_HEIGHT * 4; i += 128) __dcbf(i, lr.pBits);
        if (x->copyBuf.size() < RB_WIDTH * RB_HEIGHT) x->copyBuf.resize(RB_WIDTH * RB_HEIGHT);
        ur.left = 0; ur.top = 0; ur.right = (LONG)w; ur.bottom = (LONG)h;
        XGUntileSurface(&x->copyBuf[0], w * 4, NULL, lr.pBits, RB_WIDTH, RB_HEIGHT, &ur, 4);
        x->rbTex->UnlockRect(0);
        for (y = 0; y < hc; y++)
        {
            const u32 *src = &x->copyBuf[y * w];
            for (xx = 0; xx < w; xx++)
            {
                u32 a = s->addr + (y * w + xx) * s->bytes;
                if (a + s->bytes > H64_RDRAM_SIZE) break;
                store_pixel(ram + a, s->fmt, src[xx]);
                if (snap) store_pixel(snap + a, s->fmt, src[xx]);
            }
        }
    }
    else
    {
    x->dev->BlockUntilIdle();
    if (FAILED(s->tex->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) return;
    if (x->copyBuf.size() < RT_WIDTH * RT_HEIGHT) x->copyBuf.resize(RT_WIDTH * RT_HEIGHT);
    XGUntileSurface(&x->copyBuf[0], RT_WIDTH * 4, NULL, lr.pBits, RT_WIDTH, RT_HEIGHT, NULL, 4);
    s->tex->UnlockRect(0);
    for (y = 0; y < hc; y++)
    {
        const u32 *src = &x->copyBuf[(y * x->rtH / h) * RT_WIDTH];
        for (xx = 0; xx < w; xx++)
        {
            u32 a = s->addr + (y * w + xx) * s->bytes;
            if (a + s->bytes > H64_RDRAM_SIZE) break;
            store_pixel(ram + a, s->fmt, src[xx * x->rtW / w]);
            if (snap) store_pixel(snap + a, s->fmt, src[xx * x->rtW / w]);
        }
    }
    }
    if (x->deferNotify)
    {
        // On the graphics worker: the recompiler is the CPU thread's; tell it later.
        u32 lo = s->addr, hi = s->addr + w * hc * s->bytes;
        if (x->pendingHi <= x->pendingLo) { x->pendingLo = lo; x->pendingHi = hi; }
        else { if (lo < x->pendingLo) x->pendingLo = lo; if (hi > x->pendingHi) x->pendingHi = hi; }
    }
    else
        h64_jit_notify_write(x->sys, s->addr, w * hc * s->bytes);
    s->gpuDirty = 0;
    x->stats.copyBacks++;
}

// Texture loads from a colour image the GPU drew: copy it back first.
static void check_texture_source(Xenos *x)
{
    u32 a = x->st->texAddr & 0xFFFFFF, i;
    int found = -1;
    // The image holding the address that starts closest to it, as for the VI
    // (h64_xenos_present_vi): another image's guessed extent may cover it too.
    for (i = 0; i < FB_SLOTS; i++)
    {
        const FbSlot *s = &x->fb[i];
        if (s->tex && (s->valid || s->gpuDirty) && a >= s->addr && a < s->addr + s->width * s->height * s->bytes &&
            (found < 0 || s->addr > x->fb[found].addr ||
             (s->addr == x->fb[found].addr && s->lastUse > x->fb[found].lastUse)))
            found = (int)i;
    }
    if (found < 0 || !x->fb[found].gpuDirty) return;
    if (x->debug != 5)   // xenosdebug=5: no copy backs (diagnosis)
    {
        u64 t0 = h64_prof_now(x->sys);
        copy_back(x, found);
        x->stats.tCopyBack += h64_prof_now(x->sys) - t0;
    }
    else x->fb[found].gpuDirty = 0;
}

// ---------------------------------------------------------------- render states
// The current colour image was drawn down to line `bottom`.
static void note_drawn(Xenos *x, u32 bottom)
{
    if (x->curSlot >= 0 && x->fb[x->curSlot].drawnH < bottom) x->fb[x->curSlot].drawnH = bottom;
}

static void set_scissor(Xenos *x, float sx, float sy)
{
    RECT r;
    float xlo = (float)(x->st->scissorXlo >> 2), xhi = (float)(x->st->scissorXhi >> 2);
    float fbw = (float)(x->st->colorWidth ? x->st->colorWidth : 320);
    note_drawn(x, (x->st->scissorYhi + 3) >> 2);
    if (!(xlo == 0 && xhi >= fbw)) { xlo = ws_x(x, xlo, fbw); xhi = ws_x(x, xhi, fbw); }
    r.left = (LONG)(xlo * sx);
    r.top = (LONG)((x->st->scissorYlo >> 2) * sy);
    r.right = (LONG)(xhi * sx);
    r.bottom = (LONG)((x->st->scissorYhi >> 2) * sy);
    if (r.right > (LONG)x->rtW) r.right = (LONG)x->rtW;
    if (r.bottom > (LONG)x->rtH) r.bottom = (LONG)x->rtH;
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
    if ((test || update) && (st->depthAddr & 0xFFFFFF) != x->edramDepthAddr)
    {
        // Another depth image than the one in EDRAM: its content is unknown.
        x->dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        x->edramDepthAddr = st->depthAddr & 0xFFFFFF;
    }
    x->dev->SetRenderState(D3DRS_ZENABLE, test || update ? TRUE : FALSE);
    x->dev->SetRenderState(D3DRS_ZFUNC, test ? D3DCMP_LESSEQUAL : D3DCMP_ALWAYS);
    x->dev->SetRenderState(D3DRS_ZWRITEENABLE, update && st->zMode != 3 ? TRUE : FALSE);
    if (st->zMode == 3 || st->zMode == 2)
    {
        // Decal: the N64 passes a pixel whose depth is within the surface's
        // own slope (dz) of the stored one. Translucent surfaces (Z mode XLU)
        // also pass within dz: OoT draws its paths as translucent layers on
        // the ground mesh (Kokiri forest). A bias of a few depth slopes (the
        // N64 pixel is 3 render-target pixels wide) plus a constant keeps
        // them on their surface instead of fighting with it (Mario's shadow,
        // the paths appearing and vanishing as the camera moved).
        float bias = -0.0001f, slope = -6.0f, c[4] = { -0.0001f, -6.0f, 0.0f, 0.0f };
        x->dev->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD *)&bias);
        x->dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD *)&slope);
        x->dev->SetPixelShaderConstantF(7, c, 1);   // the same bias where the shader writes the depth (KEY_DEPTH)
    }
    else
    {
        float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        x->dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
        x->dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
        x->dev->SetPixelShaderConstantF(7, c, 1);
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
        // Coverage times alpha: the RDP's coverage becomes alpha * 8 / 256
        // subsamples and only a pixel left with none is dropped (colour and
        // depth): alpha below 32 (one half dropped parts of Conker's pub door).
        x->dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        x->dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        x->dev->SetRenderState(D3DRS_ALPHAREF, 0x20);
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

static void setup_combined(Xenos *x, int hasDepth, TexBinding *tb0, TexBinding *tb1, u32 tile, int zOut)
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
    if (zOut && hasDepth && (st->depthBlendFlags & (DB_DEPTH_TEST | DB_DEPTH_UPDATE))) flags |= KEY_DEPTH;
    if (x->texFilter == 0 && (st->rasterFlags & RS_SAMPLE_QUAD) && !(st->rasterFlags & RS_COPY)) flags |= KEY_3POINT;
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
    {
        // A full cache is emptied while a texture is looked up, which puts the
        // dummy (white) texture on both units: if that happened for unit 1,
        // unit 0 must be bound again (OoT's title logo: one white strip for a frame).
        u32 retires = x->retires;
        bind_texture(x, 0, tile, tb0);
        if (x->usesT1)
            bind_texture(x, 1, (tile + 1) & 7, tb1);
        else
        {
            // Tile + 1 is not sampled: not decoded (PD's cutscene strips: one
            // useless texture each, filling the texture arena).
            *tb1 = *tb0;
            x->dev->SetTexture(1, x->dummy);
        }
        if (x->retires != retires) bind_texture(x, 0, tile, tb0);
    }
    set_blend(x, two ? 1 : 0);
    set_depth(x, hasDepth);
    set_alpha_test(x);
    set_scissor(x, x->rtW / fbw, x->rtH / fbh);
    x->dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
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
    int i, persp = (st->rasterFlags & RS_PERSPECTIVE) != 0, wsFull;
    (void)levels;
    if (st->rasterFlags & (RS_FILL | RS_COPY)) return;
    if (!st->usePrimDepth)
        for (i = 0; i < 3; i++)
        {
            const H64RenderVertex *vv = i == 0 ? a : i == 1 ? b : c;
            if (vv->z < 0.0f || vv->z > 32767.0f) flags |= TRI_ZOUT;
        }
    if (x->stateDirty || flags != x->batchFlags || tile != x->batchTile || x->batch.size() + 3 > BATCH_VERTICES)
    {
        flush_batch(x);
        select_framebuffer(x);
        setup_combined(x, (flags & H64_TRI_ZBUFFER) != 0, &x->batchTb0, &x->batchTb1, tile, (flags & TRI_ZOUT) != 0);
        x->batchFlags = flags;
        x->batchTile = tile;
        x->stateDirty = 0;
        // tex_coord as s * A + B, once a batch (it took int-to-float
        // conversions and divisions, four times a vertex).
        tc_affine(t0->shiftS, (s32)t0->slo, x->batchTb0.w, &x->batchA[0], &x->batchB[0]);
        tc_affine(t0->shiftT, (s32)t0->tlo, x->batchTb0.h, &x->batchA[1], &x->batchB[1]);
        tc_affine(t1->shiftS, (s32)t1->slo, x->batchTb1.w, &x->batchA[2], &x->batchB[2]);
        tc_affine(t1->shiftT, (s32)t1->tlo, x->batchTb1.h, &x->batchA[3], &x->batchB[3]);
    }
    v[0] = a; v[1] = b; v[2] = c;
    {
        // 16:9 widescreen: a triangle from one screen edge exactly to the
        // other is a 2D background (MK64's sky gradient), stretched like the
        // full-width rectangles instead of narrowed.
        float fbw = (float)(st->colorWidth ? st->colorWidth : 320), lo = v[0]->x, hi = v[0]->x;
        for (i = 1; i < 3; i++) { if (v[i]->x < lo) lo = v[i]->x; if (v[i]->x > hi) hi = v[i]->x; }
        wsFull = lo > -0.6f && lo < 0.6f && hi > fbw - 0.6f && hi < fbw + 0.6f;
    }
    for (i = 0; i < 3; i++)
    {
        float w = persp && v[i]->invw > 0 ? 1.0f / v[i]->invw : 1.0f;
        float z = st->usePrimDepth ? (float)(st->primDepth >> 16) : v[i]->z;
        q[i].x = wsFull ? v[i]->x : ws_x(x, v[i]->x, (float)(st->colorWidth ? st->colorWidth : 320));
        q[i].y = v[i]->y;
        q[i].z = z / 32767.0f;
        q[i].w = w;
        if (flags & H64_TRI_SHADE)
        {
            q[i].col[0] = v[i]->r * (1.0f / 255.0f);
            q[i].col[1] = v[i]->g * (1.0f / 255.0f);
            q[i].col[2] = v[i]->b * (1.0f / 255.0f);
            q[i].col[3] = v[i]->a * (1.0f / 255.0f);
        }
        else
            q[i].col[0] = q[i].col[1] = q[i].col[2] = q[i].col[3] = 0.0f;
        q[i].u0 = v[i]->s * x->batchA[0] + x->batchB[0];
        q[i].v0 = v[i]->t * x->batchA[1] + x->batchB[1];
        q[i].u1 = v[i]->s * x->batchA[2] + x->batchB[2];
        q[i].v1 = v[i]->t * x->batchA[3] + x->batchB[3];
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
    float sx = x->rtW / fbw, sy = x->rtH / fbh;
    u32 xh = (w[0] >> 12) & 0xFFF, yh = w[0] & 0xFFF, xl = (w[1] >> 12) & 0xFFF, yl = w[1] & 0xFFF;
    D3DRECT r;
    float fx1 = (float)(xl >> 2), fx2 = (float)((xh >> 2) + 1);
    if (!((xl >> 2) == 0 && fx2 >= fbw)) { fx1 = ws_x(x, fx1, fbw); fx2 = ws_x(x, fx2, fbw); }
    r.x1 = (LONG)(fx1 * sx);
    r.y1 = (LONG)((yl >> 2) * sy);
    r.x2 = (LONG)(fx2 * sx);
    r.y2 = (LONG)(((yh >> 2) + 1) * sy);
    if (r.x2 > (LONG)x->rtW) r.x2 = (LONG)x->rtW;
    if (r.y2 > (LONG)x->rtH) r.y2 = (LONG)x->rtH;
    if (r.x1 >= r.x2 || r.y1 >= r.y2) return;
    x->stats.fills++;
    if ((st->colorAddr & 0xFFFFFF) == (st->depthAddr & 0xFFFFFF))
    {
        // Filling the depth image: a depth clear of the frame being drawn.
        // Whatever colour image the EDRAM target holds (the depth is separate
        // EDRAM): it is bound again first (unbound after a present or a copy
        // back, which used to skip the clear; harmless while every change of
        // colour image cleared the depth too).
        flush_batch(x);
        bind_n64_target(x);
        x->dev->Clear(1, &r, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        x->edramDepthAddr = st->depthAddr & 0xFFFFFF;
        return;
    }
    select_framebuffer(x);
    {
        u32 bottom = (yh >> 2) + 1, sb = (st->scissorYhi + 3) >> 2;
        note_drawn(x, bottom < sb ? bottom : sb);
    }
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

// A texture load from a colour image the GPU drew (same texel size and
// width): where its texels came from, for fb_rect_source. Perfect Dark's
// cutscenes blend the previous frame over the new one, one LoadBlock and one
// rectangle per line: decoding those ~180 strips a frame on the CPU (after
// copying the frame back) took ~25 ms; the GPU samples its own frame instead.
static void fb_load(Xenos *x, const u32 *w, int block)
{
    const H64RdpState *st = x->st;
    const H64RdpTile *lt = &st->tiles[(w[1] >> 24) & 7];
    u32 a = st->texAddr & 0xFFFFFF, i, bytes = st->texSize == 2 ? 2 : st->texSize == 3 ? 4 : 0;
    u32 sl = (w[0] >> 12) & 0xFFF, tl = w[0] & 0xFFF, sh = (w[1] >> 12) & 0xFFF, th = w[1] & 0xFFF;
    int found = -1;
    if (!bytes || st->texFmt != 0 || x->debug == 5) return;
    for (i = 0; i < FB_SLOTS; i++)
    {
        const FbSlot *s = &x->fb[i];
        if (s->valid && s->tex && s->bytes == bytes && s->width == st->texWidth && a >= s->addr &&
            a < s->addr + s->width * s->height * s->bytes && (found < 0 || s->addr > x->fb[found].addr))
            found = (int)i;
    }
    if (found < 0 || ((a - x->fb[found].addr) % bytes)) return;
    x->fbLoad.slot = found;
    x->fbLoad.slotAddr = x->fb[found].addr;
    x->fbLoad.slotUse = x->fb[found].lastUse;
    x->fbLoad.block = block;
    x->fbLoad.tmemOff = lt->offset;
    x->fbLoad.stride = lt->stride;
    if (block)
    {
        x->fbLoad.startPix = (a - x->fb[found].addr) / bytes + tl * st->texWidth + sl;
        x->fbLoad.texels = (sh - sl + 1) & 0xFFF;
        x->fbLoad.dxt = th;
    }
    else
    {
        x->fbLoad.startPix = (a - x->fb[found].addr) / bytes;
        x->fbLoad.sl = sl >> 2;
        x->fbLoad.tl = tl >> 2;
        x->fbLoad.cols = ((sh >> 2) - (sl >> 2) + 1) & 0xFFF;
        x->fbLoad.rows = (th >> 2) - (tl >> 2) + 1;
        x->fbLoad.dxt = 0;
    }
    x->fbLoad.dataGen = x->tmemDataGen;
    x->fbLoad.valid = 1;
}

// Whether a rectangle on `tile` shows texels of the last fb_load, mapped
// straight onto the source image (no flip, no shift, no palette, no wrap,
// one image row per tile row). If so, binds that image for bind_texture
// and gives map[0..1] = the source pixel of tile texel (0, 0), map[2..3] =
// texture units per source pixel.
static int fb_rect_source(Xenos *x, const H64RdpTile *t, int flip, float s, float tc, float dsdx, float dtdy, float spanX,
                          float spanY, float *map)
{
    const H64RdpState *st = x->st;
    const FbSlot *fs;
    float s0, s1, t0, t1;
    s32 rel, rb, row, xb, yb, width;
    int single, multi;
    if (!x->fbLoad.valid || x->fbLoad.dataGen != x->tmemDataGen || flip || dsdx <= 0.0f || dtdy < 0.0f) return 0;
    fs = &x->fb[x->fbLoad.slot];
    if (!fs->valid || !fs->tex || fs->addr != x->fbLoad.slotAddr || fs->lastUse != x->fbLoad.slotUse ||
        x->fbLoad.slot == x->curSlot)
        return 0;
    if (t->fmt != 0 || t->size != (fs->bytes == 2 ? 2 : 3) || (st->rasterFlags & RS_TLUT) || t->shiftS || t->shiftT) return 0;
    width = (s32)fs->width;
    s0 = s / 32.0f - t->slo / 4.0f;
    t0 = tc / 32.0f - t->tlo / 4.0f;
    s1 = s0 + spanX * dsdx;
    t1 = t0 + spanY * dtdy;
    if (s0 < 0.0f || t0 < 0.0f) return 0;
    if (t->maskS && s1 > (float)(1 << t->maskS)) return 0;
    if (t->maskT && t1 > (float)(1 << t->maskT)) return 0;
    row = (s32)floorf(t0);
    single = (s32)floorf(t1 - 0.001f) == row;   // one tile row
    rel = (s32)t->offset - (s32)x->fbLoad.tmemOff;
    if (rel < 0 || (rel & 1)) return 0;
    rb = single ? row : 0;
    if (x->fbLoad.block)
    {
        // Linear texels from startPix. Odd tile rows are read with swapped
        // words: only right when the load swapped them too (dxt).
        s32 p;
        multi = !single;
        if ((multi || (row & 1)) && !x->fbLoad.dxt) return 0;
        if (multi && t->stride != (u32)width * 2) return 0;
        p = rel / 2 + rb * (s32)t->stride / 2;
        if (p + (s32)ceilf(s1) > (s32)x->fbLoad.texels + (multi ? (s32)(t1 + 1.0f) * width : 0)) return 0;
        p += (s32)x->fbLoad.startPix;
        xb = p % width;
        yb = p / width;
    }
    else
    {
        // Lines of `cols` texels, `stride` bytes apart in TMEM, from (sl, tl).
        s32 r, c, b = rel + rb * (s32)t->stride, ls = (s32)x->fbLoad.stride;
        if (!ls) return 0;
        if (!single && t->stride != (u32)ls) return 0;
        r = b / ls;
        c = (b % ls) / 2;
        if (((r ^ rb) & 1) || c + (s32)ceilf(s1) > (s32)x->fbLoad.cols || r + (single ? 1 : (s32)ceilf(t1)) > (s32)x->fbLoad.rows)
            return 0;
        xb = (s32)(x->fbLoad.startPix % (u32)width) + (s32)x->fbLoad.sl + c;
        yb = (s32)(x->fbLoad.startPix / (u32)width) + (s32)x->fbLoad.tl + r;
    }
    if (xb + s1 > (float)width + 0.001f) return 0;   // a row of the tile must stay on one image row
    map[0] = (float)xb;
    map[1] = (float)(yb - rb);
    map[2] = x->uMax / (float)width;
    map[3] = x->vMax / (float)(fs->height ? fs->height : 240);
    x->fbTex = fs->tex;
    x->fbTexels[0] = (float)width / x->uMax;
    x->fbTexels[1] = (float)(fs->height ? fs->height : 240) / x->vMax;
    return 1;
}

static void tex_rect(Xenos *x, const u32 *w, int flip)
{
    int fullWidth = 0;   // set below: a rectangle across the whole image (a background) stays full width
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
    int i, fbSrc;
    float fbMap[4];
    if (copy)
    {
        dsdx /= 4.0f;
        xh += 1.0f;
        yh += 1.0f;
    }
    if (xh <= xl || yh <= yl) return;
    select_framebuffer(x);
    x->win.active = 0;
    fbSrc = fb_rect_source(x, t0, flip, s, t, dsdx, dtdy, xh - xl, yh - yl, fbMap);
    if (!fbSrc && !t0->shiftS && !t0->shiftT)
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
        set_scissor(x, x->rtW / fbw, x->rtH / fbh);
    }
    else
    {
        setup_combined(x, 1, &tb0, &tb1, tile, 0);
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
    fullWidth = xl <= 0.5f && xh >= (float)(st->colorWidth ? st->colorWidth : 320) - 1.0f;
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
        q[i].x = fullWidth ? px : ws_x(x, px, (float)(st->colorWidth ? st->colorWidth : 320));
        q[i].y = py;
        q[i].z = z;
        q[i].w = 1.0f;
        q[i].col[0] = q[i].col[1] = q[i].col[2] = q[i].col[3] = 0.0f;
        q[i].u0 = tex_coord(ss, t0->shiftS, (s32)t0->slo + 4 * tb0.ox, tb0.w);
        q[i].v0 = tex_coord(tt, t0->shiftT, (s32)t0->tlo + 4 * tb0.oy, tb0.h);
        q[i].u1 = tex_coord(ss, t1->shiftS, t1->slo, tb1.w);
        q[i].v1 = tex_coord(tt, t1->shiftT, t1->tlo, tb1.h);
        if (fbSrc)
        {
            // Tile texel -> pixel of the source image -> its texture.
            q[i].u0 = (fbMap[0] + ss / 32.0f - t0->slo / 4.0f) * fbMap[2];
            q[i].v0 = (fbMap[1] + tt / 32.0f - t0->tlo / 4.0f) * fbMap[3];
        }
    }
    x->win.active = 0;
    x->fbTex = NULL;
    x->dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(XVtx));
    x->stats.rects++;
    x->stats.draws++;
    if (fbSrc) x->stats.fbTexRects++;
}

// A raw RDP triangle (LLE graphics): drawn from its rebuilt vertices.
static void raw_triangle(Xenos *x, const u32 *w, u32 op)
{
    H64RenderVertex v[3];
    h64_rdp_decode_triangle(w, (x->st->rasterFlags & RS_PERSPECTIVE) != 0, v);
    xenos_triangle(x, &v[0], &v[1], &v[2], op & 7, (w[0] >> 16) & 7, ((w[0] >> 19) & 7) + 1);
}

// A save state was loaded: the frames and the N64 target describe the old
// RDRAM. Textures stay (their keys hash the texels they were made from).
static void xenos_reset(void *user)
{
    Xenos *x = (Xenos *)user;
    u32 i;
    x->batch.clear();
    for (i = 0; i < FB_SLOTS; i++)
    {
        x->fb[i].valid = 0;
        x->fb[i].gpuDirty = 0;   // never copy the old frame back over the loaded RDRAM
    }
    x->curSlot = -1;
    x->edramOwner = -1;
    x->edramDepthAddr = 0xFFFFFFFFu;
    x->shownSlot = -1;
    x->targetBound = 0;
    x->tmemGen++;
    x->fbLoad.valid = 0;
    x->stateDirty = 1;
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
    if (op == 0x30 || op == 0x33 || op == 0x34)
    {
        x->tmemDataGen++;
        x->fbLoad.valid = 0;
        if (op != 0x30) fb_load(x, w, op == 0x33);
    }
    // State and TMEM: the software RDP (state only).
    t0 = h64_prof_now(x->sys);
    h64_rdp_command(x->sys, words, count);
    x->stats.tState += h64_prof_now(x->sys) - t0;
    if (op == 0x30 || op == 0x33 || op == 0x34) x->stats.tLoads += h64_prof_now(x->sys) - t0;
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

static void show_rdram_frame(Xenos *x, const u32 *vi, u32 origin, u32 width, int bpp32)
{
    u32 h = fb_height(width);
    ViGeom g;
    IDirect3DTexture9 *tex = upload_rdram(x, origin, width, bpp32);
    if (!tex) return;
    // The upload starts at the origin: no offset.
    draw_display(x, tex, vi_geometry(vi, &g) ? &g : NULL, 0.0f, 0.0f, 1.0f / RT_WIDTH, 1.0f / RT_HEIGHT, width, h);
    x->shown = tex;
    x->shownTiled = 0;
    x->shownU = (float)width / RT_WIDTH;
    x->shownV = (float)h / RT_HEIGHT;
}

void h64_xenos_present(H64Renderer *r)
{
    h64_xenos_present_vi(r, X(r)->sys->vi.regs);
}

void h64_xenos_present_vi(H64Renderer *r, const u32 *vi)
{
    Xenos *x = X(r);
    flush_batch(x);
    x->stateDirty = 1;
    u32 origin = vi[1] & 0xFFFFFF, width = vi[2] & 0xFFF, type = vi[0] & 3, i;
    int found = -1;
    if (x->curSlot >= 0 && x->targetBound) resolve_current(x);
    // The image holding the origin that starts closest to it (the most recently
    // drawn one at equal addresses): another image's guessed extent can cover
    // it too. DK64's stale 640x480 intro buffers span its 320x240 ones; the
    // first match showed a black frame every other game frame. Conker's
    // buffers are closer than a guessed 240 lines: the most recently drawn
    // one, which covered the other, gave the same black frames.
    for (i = 0; i < FB_SLOTS; i++)
    {
        const FbSlot *s = &x->fb[i];
        if (s->valid && s->tex && origin >= s->addr && origin < s->addr + s->width * s->height * s->bytes &&
            (found < 0 || s->addr > x->fb[found].addr ||
             (s->addr == x->fb[found].addr && s->lastUse > x->fb[found].lastUse)))
            found = (int)i;
    }
    if (x->debug == 7 && x->presentCount >= 600 && x->presentCount < 624)
    {
        // xenosdebug=7: every present for a moment (frames shown out of order).
        H64_INFO("[xenos] present %u: origin %06X -> slot %d (addr %06X, last drawn at present %u, use %u), cur slot %d",
                 x->presentCount, origin, found, found >= 0 ? x->fb[found].addr : 0, found >= 0 ? x->fb[found].drawnAt : 0,
                 found >= 0 ? x->fb[found].lastUse : 0, x->curSlot);
    }
    if (x->debug == 6 && (x->presentCount % 60) == 0)
    {
        // xenosdebug=6: what the VI shows, once a second.
        H64_INFO("[xenos] VI origin %06X width %u type %u -> slot %d", origin, width, type, found);
        for (i = 0; i < FB_SLOTS; i++)
            if (x->fb[i].tex)
                H64_INFO("[xenos]   slot %u: addr %06X %ux%u bytes %u valid %d drawnH %u lastUse %u", i, x->fb[i].addr, x->fb[i].width,
                         x->fb[i].height, x->fb[i].bytes, x->fb[i].valid, x->fb[i].drawnH, x->fb[i].lastUse);
    }
    x->dev->SetRenderTarget(0, x->backBuffer);
    x->dev->SetDepthStencilSurface(NULL);
    x->targetBound = 0;
    x->dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    if (type < 2)
        ;
    else if (found >= 0)
    {
        const FbSlot *fs = &x->fb[found];
        ViGeom g;
        u32 bpr = (fs->width ? fs->width : 320) * (fs->bytes ? fs->bytes : 2), off = origin - fs->addr;
        float oy = (float)(off / bpr), ox = (float)((off % bpr) / (fs->bytes ? fs->bytes : 2));
        int haveGeom = vi_geometry(vi, &g);
        if (haveGeom && g.serrate) oy = (float)((u32)oy & ~1u);   // interlaced: both fields are in the image
        draw_display(x, fs->showFront && fs->front ? fs->front : fs->tex, haveGeom ? &g : NULL, ox, oy,
                     x->uMax / (fs->width ? fs->width : 320), x->vMax / (fs->height ? fs->height : 240), fs->width, fs->height);
        x->shown = fs->showFront && fs->front ? fs->front : fs->tex;
        x->shownU = x->uMax;
        x->shownV = x->vMax;
        x->shownTiled = 1;
    }
    else if (x->drewFrame || ++x->blankPresents > 300)
        show_rdram_frame(x, vi, origin, width, type == 3);
    // else: at boot the VI often shows RDRAM the game still uses for other data
    // (noise); stay black until the RDP draws a frame, or for 300 VIs at most
    // (games that draw their first images with the CPU).
    {
        // The VI moved to another image: the slots it left show their newest picture again.
        u32 k;
        for (k = 0; k < FB_SLOTS; k++)
            if ((int)k != found) x->fb[k].showFront = 0;
        x->shownSlot = found;
    }
    if (x->overlay) x->overlay(x->overlayUser, x->dev);
    x->dev->Present(NULL, NULL, NULL, NULL);
    x->stats.presents++;
    x->presentCount++;
    // The next draw rebinds the N64 target and restores its content.
    x->curSlot = -1;
}

void h64_xenos_set_overlay(H64Renderer *r, void (*fn)(void *user, IDirect3DDevice9 *dev), void *user)
{
    Xenos *x = X(r);
    x->overlay = fn;
    x->overlayUser = user;
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
        { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        { 0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    D3DSURFACE_PARAMETERS sp;
    Xenos *x = new Xenos;
    int i;
    x->api.user = x;
    x->api.rdp = xenos_rdp;
    x->api.triangle = xenos_triangle;
    x->api.reset = xenos_reset;
    x->sys = sys;
    x->dev = dev;
    sys->options.rdpStateOnly = 1;
    x->st = h64_rdp_state(sys);
    x->combineRaw = 0;
    x->textureBytes = 0;
    x->poolBytes = 0;
    x->retires = 0;
    x->presentCount = 0;
    x->arenaHead = 0;
    x->arena = (u8 *)XPhysicalAlloc(ARENA_BYTES, MAXULONG_PTR, 4096, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!x->arena) H64_WARN("[xenos] no texture arena (%u MB): CreateTexture for every texture", ARENA_BYTES >> 20);
    x->stateDirty = 1;
    x->batchFlags = x->batchTile = 0xFFFFFFFF;
    x->tmemGen = 1;
    x->tmemDataGen = 0;
    memset(&x->fbLoad, 0, sizeof(x->fbLoad));
    x->fbTex = NULL;
    x->usesT1 = 1;
    memset(x->memo, 0, sizeof(x->memo));
    x->batch.reserve(BATCH_VERTICES);
    x->curSlot = -1;
    x->edramOwner = -1;
    x->edramDepthAddr = 0xFFFFFFFFu;
    x->shownSlot = -1;
    x->drewFrame = 0;
    x->blankPresents = 0;
    x->overlay = NULL;
    x->overlayUser = NULL;
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
    sp.Base = EDRAM_READBACK;
    sp.HierarchicalZBase = 0xFFFFFFFF;
    x->rbSurf = NULL;
    x->rbTex = NULL;
    if (FAILED(dev->CreateRenderTarget(RB_WIDTH, RB_HEIGHT, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &x->rbSurf, &sp))) x->rbSurf = NULL;
    if (FAILED(dev->CreateTexture(RB_WIDTH, RB_HEIGHT, 1, D3DUSAGE_CPU_CACHED_MEMORY, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &x->rbTex, NULL)))
        x->rbTex = NULL;
    dev->CreateVertexDeclaration(decl, &x->decl);
    x->vs = compile_vs(dev, s_vsSource);
    x->psCopy = compile_ps(dev, s_psCopySource);
    x->psSmooth = compile_ps(dev, s_psSmoothSource);
    x->smooth = 1;
    x->scale = 3;
    x->rtW = RT_WIDTH;
    x->rtH = RT_HEIGHT;
    x->uMax = x->vMax = 1.0f;
    x->texFilter = 1;
    x->sharpen = x->blur = x->screen = x->aspect = 0;
    x->deferNotify = 0;
    x->pendingLo = x->pendingHi = 0;
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
    retire_all_textures(x);   // waits for the GPU
    // Nothing of this renderer may stay bound to the device once released (the
    // 360 device keeps no reference): the next game's renderer, created on the
    // same device, crashed at its first draws.
    x->dev->SetTexture(0, NULL);
    x->dev->SetTexture(1, NULL);
    x->dev->SetPixelShader(NULL);
    x->dev->SetVertexShader(NULL);
    x->dev->SetVertexDeclaration(NULL);
    x->dev->BlockUntilIdle();
    {
        std::map<u32, std::vector<IDirect3DTexture9 *> >::iterator pit;
        size_t k;
        for (pit = x->pool.begin(); pit != x->pool.end(); ++pit)
            for (k = 0; k < pit->second.size(); k++) pit->second[k]->Release();
        x->pool.clear();
    }
    {
        u32 c, k;
        for (c = 0; c < ARENA_CHUNKS; c++)
            for (k = 0; k < x->arenaHeaders[c].size(); k++) delete x->arenaHeaders[c][k];
        if (x->arena) XPhysicalFree(x->arena);
        x->arena = NULL;
    }
    for (it = x->shaders.begin(); it != x->shaders.end(); ++it)
        if (it->second) it->second->Release();
    for (i = 0; i < FB_SLOTS; i++)
    {
        if (x->fb[i].tex) x->fb[i].tex->Release();
        if (x->fb[i].front) x->fb[i].front->Release();
    }
    for (i = 0; i < 2; i++)
        if (x->cpuFb[i]) x->cpuFb[i]->Release();
    if (x->dummy) x->dummy->Release();
    if (x->psCopy) x->psCopy->Release();
    if (x->psFill) x->psFill->Release();
    if (x->psFallback) x->psFallback->Release();
    if (x->psSmooth) x->psSmooth->Release();
    {
        std::map<int, IDirect3DPixelShader9 *>::iterator d;
        for (d = x->psDisplay.begin(); d != x->psDisplay.end(); ++d) if (d->second) d->second->Release();
        x->psDisplay.clear();
    }
    if (x->vs) x->vs->Release();
    if (x->decl) x->decl->Release();
    if (x->n64Color) x->n64Color->Release();
    if (x->n64Depth) x->n64Depth->Release();
    if (x->rbSurf) x->rbSurf->Release();
    if (x->rbTex) x->rbTex->Release();
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

void h64_xenos_set_worker(H64Renderer *r, int on)
{
    Xenos *x = X(r);
    x->deferNotify = on;
    if (!on && x->pendingHi > x->pendingLo)
    {
        h64_jit_notify_write(x->sys, x->pendingLo, x->pendingHi - x->pendingLo);
        x->pendingLo = x->pendingHi = 0;
    }
}

void h64_xenos_set_smooth(H64Renderer *r, int on)
{
    X(r)->smooth = on;
}

void h64_xenos_set_options(H64Renderer *r, const H64XenosOptions *o)
{
    Xenos *x = X(r);
    int scale = o->scale < 1 ? 1 : o->scale > 3 ? 3 : o->scale;
    x->smooth = o->smooth;
    x->sharpen = o->sharpen < 0 || o->sharpen > 2 ? 0 : o->sharpen;
    x->blur = o->blur < 0 || o->blur > 2 ? 0 : o->blur;
    x->screen = o->screen < 0 || o->screen > 5 ? 0 : o->screen;
    if (o->aspect != x->aspect) x->stateDirty = 1;   // the scissor moves in widescreen
    x->aspect = o->aspect < 0 || o->aspect > 2 ? 0 : o->aspect;
    if (o->texFilter != x->texFilter)
    {
        x->texFilter = o->texFilter < 0 || o->texFilter > 2 ? 1 : o->texFilter;
        x->stateDirty = 1;
    }
    if (scale != x->scale)
    {
        x->scale = scale;
        x->rtW = RT_WIDTH * (u32)scale / 3;
        x->rtH = RT_HEIGHT * (u32)scale / 3;
        x->uMax = (float)x->rtW / RT_WIDTH;
        x->vMax = (float)x->rtH / RT_HEIGHT;
        x->targetBound = 0;   // the viewport is set when the N64 target is bound again
        x->stateDirty = 1;
    }
}

void h64_xenos_set_debug(H64Renderer *r, int mode)
{
    Xenos *x = X(r);
    x->debugFrom = 2500;
    if (mode >= 8000) { x->debugFrom = (u32)(mode - 8000); mode = 8; }
    x->debug = mode;
    x->shaders.clear();   // leaks the compiled shaders: debug only
    x->shaderT1.clear();
}

void h64_xenos_stats(H64Renderer *r, H64XenosStats *out, int reset)
{
    Xenos *x = X(r);
    *out = x->stats;
    if (reset) memset(&x->stats, 0, sizeof(x->stats));
}
