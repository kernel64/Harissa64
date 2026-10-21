// Harissa64 V2 - screen drawing for the front end (see xb_ui.h).
#include "xb_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "../../core/common/h64_types.h"
#include "xb_ui_atlas.h"

// ---- Device resources ----
// One vertex: position (pixels, turned into clip space here), colour, t and p.
// Shapes: t = (x, y from the shape's centre, 0, half ring width or 0),
//         p = (half width, half height, corner radius, edge width).
// Atlas:  t = (u, v, 1, 0), p = (screen pixels per unit of the field, edge offset, 0, 1).
struct UiVtx
{
    float x, y;
    D3DCOLOR color;
    float t[4];
    float p[4];
};

static const D3DVERTEXELEMENT9 s_declElems[] = {
    { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 8, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
    { 0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 28, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
    D3DDECL_END()
};

static const char s_uiVs[] =
    "struct VIN { float2 pos : POSITION; float4 col : COLOR0; float4 t : TEXCOORD0; float4 p : TEXCOORD1; };\n"
    "struct VOUT { float4 pos : POSITION; float4 col : COLOR0; float4 t : TEXCOORD0; float4 p : TEXCOORD1; };\n"
    "VOUT main(VIN v) { VOUT o; o.pos = float4(v.pos, 0.0, 1.0); o.col = v.col; o.t = v.t; o.p = v.p; return o; }\n";

// Rounded box distance (negative inside), optionally turned into a ring; or
// the atlas's distance field; or a picture (t.z = 2: the image on s1 over the
// box, its corners rounded like a shape's). Kept short: the Xenos compiler
// ran out of registers on long display shaders.
static const char s_uiPs[] =
    "sampler atlas : register(s0);\n"
    "sampler image : register(s1);\n"
    "float4 main(float4 col : COLOR0, float4 t : TEXCOORD0, float4 p : TEXCOORD1) : COLOR\n"
    "{\n"
    "  float2 q = abs(t.xy) - p.xy + p.z;\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - p.z;\n"
    "  d = t.w > 0.0 ? abs(d + t.w) - t.w : d;\n"
    "  float shape = saturate(0.5 - d / p.w);\n"
    "  shape = shape * shape * (3.0 - 2.0 * shape);\n"
    "  float text = saturate((tex2D(atlas, t.xy).x - 0.5) * p.x + p.y);\n"
    "  float4 img = t.z > 1.5 ? tex2D(image, t.xy / (2.0 * p.xy) + 0.5) : float4(1.0, 1.0, 1.0, 1.0);\n"
    "  return float4(col.rgb * img.rgb, col.a * img.a * (t.z > 0.5 && t.z < 1.5 ? text : shape));\n"
    "}\n";

static IDirect3DDevice9 *s_dev;
static IDirect3DTexture9 *s_atlas;
static IDirect3DVertexShader9 *s_vs;
static IDirect3DPixelShader9 *s_ps;
static IDirect3DVertexDeclaration9 *s_decl;
static int s_ready;   // 1: resources made, -1: failed (the UI draws nothing)

#define UI_MAX_QUADS 256
static UiVtx s_vtx[UI_MAX_QUADS * 6];
static int s_quads;

static LPD3DXBUFFER compile(const char *src, const char *profile)
{
    LPD3DXBUFFER code = NULL, err = NULL;
    if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", profile, 0, &code, &err, NULL)))
    {
        H64_ERROR("[ui] %s shader compile failed: %s", profile, err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return NULL;
    }
    if (err) err->Release();
    return code;
}

static int make_atlas(IDirect3DDevice9 *dev)
{
    D3DLOCKED_RECT lr;
    const unsigned char *src = s_atlasRle, *end = s_atlasRle + sizeof(s_atlasRle);
    int x = 0, y = 0;
    u8 *row;
    if (FAILED(dev->CreateTexture(UI_ATLAS_W, UI_ATLAS_H, 1, 0, D3DFMT_LIN_L8, D3DPOOL_DEFAULT, &s_atlas, NULL)) || !s_atlas)
        return 0;
    if (FAILED(s_atlas->LockRect(0, &lr, NULL, 0))) return 0;
    row = (u8 *)lr.pBits;
    // Run-length decoding straight into the texture, row by row (the pitch may exceed the width).
    while (src < end && y < UI_ATLAS_H)
    {
        int c = *src++, n, i;
        if (c < 128)
        {
            n = c + 1;
            for (i = 0; i < n && src < end; i++)
            {
                row[x] = *src++;
                if (++x == UI_ATLAS_W) { x = 0; y++; row = (u8 *)lr.pBits + y * lr.Pitch; if (y == UI_ATLAS_H) break; }
            }
        }
        else
        {
            u8 v = src < end ? *src++ : 0;
            n = c - 126;
            for (i = 0; i < n; i++)
            {
                row[x] = v;
                if (++x == UI_ATLAS_W) { x = 0; y++; row = (u8 *)lr.pBits + y * lr.Pitch; if (y == UI_ATLAS_H) break; }
            }
        }
    }
    s_atlas->UnlockRect(0);
    return y == UI_ATLAS_H;
}

static int init_resources(IDirect3DDevice9 *dev)
{
    LPD3DXBUFFER vs = compile(s_uiVs, "vs_3_0"), ps = compile(s_uiPs, "ps_3_0");
    if (vs) { dev->CreateVertexShader((const DWORD *)vs->GetBufferPointer(), &s_vs); vs->Release(); }
    if (ps) { dev->CreatePixelShader((const DWORD *)ps->GetBufferPointer(), &s_ps); ps->Release(); }
    dev->CreateVertexDeclaration(s_declElems, &s_decl);
    if (!make_atlas(dev)) H64_ERROR("[ui] atlas texture failed");
    if (!s_vs || !s_ps || !s_decl || !s_atlas)
    {
        H64_ERROR("[ui] initialisation failed (vs %p ps %p decl %p atlas %p)", (void *)s_vs, (void *)s_ps, (void *)s_decl, (void *)s_atlas);
        return -1;
    }
    H64_INFO("[ui] ready (atlas %dx%d)", UI_ATLAS_W, UI_ATLAS_H);
    return 1;
}

float UiTime(void)
{
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
    {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    QueryPerformanceCounter(&t);
    return (float)((double)(t.QuadPart - start.QuadPart) / (double)freq.QuadPart);
}

static void flush(void)
{
    if (!s_quads || !s_dev) return;
    s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, s_quads * 2, s_vtx, sizeof(UiVtx));
    s_quads = 0;
}

void UiBegin(IDirect3DDevice9 *dev)
{
    D3DVIEWPORT9 vp;
    if (!s_ready) s_ready = init_resources(dev);
    s_dev = s_ready > 0 ? dev : NULL;
    s_quads = 0;
    if (!s_dev) return;
    UiTime();
    vp.X = 0; vp.Y = 0; vp.Width = UI_WIDTH; vp.Height = UI_HEIGHT; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetVertexDeclaration(s_decl);
    dev->SetVertexShader(s_vs);
    dev->SetPixelShader(s_ps);
    // Unit 1 holds the pictures (UiImage); the atlas when none is drawn, never nothing.
    {
        DWORD u;
        for (u = 0; u < 2; u++)
        {
            dev->SetTexture(u, s_atlas);
            dev->SetSamplerState(u, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(u, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(u, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(u, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(u, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        }
    }
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
}

void UiEnd(void)
{
    if (!s_dev) return;
    flush();
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    s_dev = NULL;
}

// ---- Quads ----
static void vtx(UiVtx *v, float x, float y, D3DCOLOR c, float t0, float t1, float t2, float t3, const float *p)
{
    v->x = x * (2.0f / UI_WIDTH) - 1.0f;
    v->y = 1.0f - y * (2.0f / UI_HEIGHT);
    v->color = c;
    v->t[0] = t0; v->t[1] = t1; v->t[2] = t2; v->t[3] = t3;
    v->p[0] = p[0]; v->p[1] = p[1]; v->p[2] = p[2]; v->p[3] = p[3];
}

// Corners (x0, y0)-(x1, y1) with t.xy from (u0, v0) to (u1, v1); top colour ct, bottom cb.
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float mode, float ring,
                 const float *p, D3DCOLOR ct, D3DCOLOR cb)
{
    UiVtx *v;
    if (!s_dev) return;
    if (s_quads == UI_MAX_QUADS) flush();
    v = &s_vtx[s_quads * 6];
    vtx(&v[0], x0, y0, ct, u0, v0, mode, ring, p);
    vtx(&v[1], x1, y0, ct, u1, v0, mode, ring, p);
    vtx(&v[2], x0, y1, cb, u0, v1, mode, ring, p);
    v[3] = v[1];
    vtx(&v[4], x1, y1, cb, u1, v1, mode, ring, p);
    v[5] = v[2];
    s_quads++;
}

// A rounded box (x, y, w, h), radius r, edge width soft; ring: half the ring's width (0: filled).
static void shape(float x, float y, float w, float h, float r, float soft, float ring, D3DCOLOR ct, D3DCOLOR cb)
{
    float p[4], pad = soft + 1.0f, hw = w * 0.5f, hh = h * 0.5f;
    if (w <= 0 || h <= 0) return;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    p[0] = hw; p[1] = hh; p[2] = r; p[3] = soft;   // a ring lies inside the edge (the shader offsets it)
    quad(x - pad, y - pad, x + w + pad, y + h + pad, -hw - pad, -hh - pad, hw + pad, hh + pad, 0.0f, ring, p, ct, cb);
}

static D3DCOLOR lerp_color(D3DCOLOR a, D3DCOLOR b, float t)
{
    u32 r = 0;
    int s;
    for (s = 0; s < 32; s += 8)
    {
        float ca = (float)((a >> s) & 0xFF), cbv = (float)((b >> s) & 0xFF);
        float v = ca + (cbv - ca) * t + 0.5f;
        r |= (u32)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v) << s;
    }
    return r;
}

void UiRoundRect(float x, float y, float w, float h, float r, D3DCOLOR top, D3DCOLOR bottom)
{
    // The quad is padded for the anti-aliased edge: extend the gradient to the padded corners.
    float k = h > 0 ? 2.0f / h : 0.0f;
    shape(x, y, w, h, r, 1.0f, 0.0f, lerp_color(top, bottom, -k), lerp_color(top, bottom, 1.0f + k));
}

void UiRect(float x, float y, float w, float h, D3DCOLOR color) { shape(x, y, w, h, 0.0f, 1.0f, 0.0f, color, color); }

void UiRoundRing(float x, float y, float w, float h, float r, float thick, D3DCOLOR color)
{
    shape(x, y, w, h, r, 1.0f, thick * 0.5f, color, color);
}

void UiShadow(float x, float y, float w, float h, float r, float blur, D3DCOLOR color)
{
    shape(x, y, w, h, r + blur * 0.5f, blur, 0.0f, color, color);
}

void UiCircle(float cx, float cy, float r, D3DCOLOR color) { shape(cx - r, cy - r, r * 2, r * 2, r, 1.0f, 0.0f, color, color); }

static D3DCOLOR with_alpha(D3DCOLOR c, float a)
{
    float v = (float)(c >> 24) * a;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (c & 0x00FFFFFF) | ((u32)(v + 0.5f) << 24);
}

void UiBackdrop(void)
{
    shape(0, 0, UI_WIDTH, UI_HEIGHT, 0.0f, 1.0f, 0.0f, UI_COL_BG_TOP, UI_COL_BG_BOTTOM);
    // Two soft glows: warm at the top right, cool at the bottom left.
    shape(860, -420, 760, 760, 380, 420, 0.0f, D3DCOLOR_ARGB(46, 234, 76, 58), D3DCOLOR_ARGB(46, 234, 76, 58));
    shape(-380, 420, 720, 720, 360, 420, 0.0f, D3DCOLOR_ARGB(26, 70, 90, 200), D3DCOLOR_ARGB(26, 70, 90, 200));
}

void UiDim(void) { UiRect(0, 0, UI_WIDTH, UI_HEIGHT, UI_COL_DIM); }

D3DCOLOR UiAlpha(D3DCOLOR c, float a) { return with_alpha(c, a); }

// ---- Pictures ----
IDirect3DTexture9 *UiImageCreate(IDirect3DDevice9 *dev, const unsigned char *rgba, int w, int h)
{
    IDirect3DTexture9 *tex = NULL;
    D3DLOCKED_RECT lr;
    int x, y;
    if (!dev || !rgba || w <= 0 || h <= 0) return NULL;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL)) || !tex) return NULL;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
    {
        tex->Release();
        return NULL;
    }
    // Bytes R, G, B, A to the texture's A, R, G, B (memory order of a big-endian ARGB word).
    for (y = 0; y < h; y++)
    {
        const unsigned char *s = rgba + (size_t)y * w * 4;
        unsigned char *d = (unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
        for (x = 0; x < w; x++, s += 4, d += 4)
        {
            d[0] = s[3];
            d[1] = s[0];
            d[2] = s[1];
            d[3] = s[2];
        }
    }
    tex->UnlockRect(0);
    return tex;
}

void UiImageRelease(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex)
{
    if (!tex) return;
    // The device keeps no reference: never release what the GPU may still read.
    if (dev)
    {
        dev->SetTexture(1, s_atlas ? (IDirect3DBaseTexture9 *)s_atlas : NULL);
        dev->BlockUntilIdle();
    }
    tex->Release();
}

void UiImage(IDirect3DTexture9 *tex, float x, float y, float w, float h, float r, D3DCOLOR color)
{
    float p[4], pad = 2.0f, hw = w * 0.5f, hh = h * 0.5f;
    if (!s_dev || !tex || w <= 0 || h <= 0) return;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    flush();
    s_dev->SetTexture(1, tex);
    p[0] = hw; p[1] = hh; p[2] = r; p[3] = 1.0f;
    quad(x - pad, y - pad, x + w + pad, y + h + pad, -hw - pad, -hh - pad, hw + pad, hh + pad, 2.0f, 0.0f, p, color, color);
    flush();
    s_dev->SetTexture(1, s_atlas);
}

void UiPanel(float x, float y, float w, float h, float r)
{
    UiShadow(x, y + 12, w, h, r, 48, D3DCOLOR_ARGB(150, 0, 0, 0));
    UiRoundRect(x, y, w, h, r, UI_COL_PANEL_TOP, UI_COL_PANEL);
    UiRoundRing(x, y, w, h, r, 1.0f, UI_COL_LINE);
}

// ---- Text ----
// Text is UTF-8. A malformed byte shows as '?', as do characters the atlas
// does not have. Lines holding Arabic are shown right to left (ui_bidi).
#define UI_BIDI_MAX 512

// The next code point of [*pp, end); moves *pp past it.
static u32 utf8_next(const char **pp, const char *end)
{
    const u8 *p = (const u8 *)*pp;
    u32 c = p[0];
    int n, i;
    if (c < 0x80) { *pp += 1; return c; }
    if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; }
    else { *pp += 1; return '?'; }
    if ((const char *)p + 1 + n > end) { *pp += 1; return '?'; }
    for (i = 1; i <= n; i++)
    {
        if ((p[i] & 0xC0) != 0x80) { *pp += 1; return '?'; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *pp += n + 1;
    return c;
}

// Bytes of the UTF-8 character starting at `s` (1 for a malformed byte).
static int utf8_len(const char *s)
{
    const char *p = s;
    utf8_next(&p, s + strlen(s));
    return (int)(p - s);
}

static const UiAtlasGlyph *glyph(int font, u32 c)
{
    int lo = 95, hi = UI_ATLAS_COUNT - 1;
    const UiAtlasGlyph *g = s_atlasGlyph[font ? 1 : 0];
    if (c >= 32 && c < 127) return &g[c - 32];   // ASCII: the first 95, in order
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        if (s_atlasCode[mid] == c) return &g[mid];
        if (s_atlasCode[mid] < c) lo = mid + 1;
        else hi = mid - 1;
    }
    return &g['?' - 32];
}

static float line_width(int font, float size, const char *s, int n)
{
    float w = 0, k = size / UI_ATLAS_BASE;
    const char *end = s + n;
    while (s < end) w += glyph(font, utf8_next(&s, end))->advance * k;
    return w;
}

// ---- Right to left ----
static int is_rtl(u32 c) { return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF); }
// Letters and digits of the left-to-right scripts.
static int is_ltr(u32 c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x0590 && c != 0xD7 && c != 0xF7);
}
static u32 mirror(u32 c)
{
    switch (c)
    {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '<': return '>';
    case '>': return '<';
    case 0x2039: return 0x203A;
    case 0x203A: return 0x2039;
    case 0xAB: return 0xBB;
    case 0xBB: return 0xAB;
    }
    return c;
}

// Puts a line holding right-to-left letters in display order (a simple form
// of the Unicode bidirectional algorithm, right-to-left paragraph): runs of
// Latin or Cyrillic words and numbers keep their order inside, everything else
// reads from the right; signs between two left-to-right runs belong to them.
static void ui_bidi(u32 *c, int n)
{
    u8 type[UI_BIDI_MAX];   // 1: left to right
    int i, j;
    for (i = 0; i < n; i++) type[i] = is_ltr(c[i]) ? 1 : 0;
    for (i = 0; i < n; i++)
    {
        if (type[i] || is_rtl(c[i])) continue;
        // Neutral: left to right when the strong characters on both sides are.
        for (j = i; j < n && !type[j] && !is_rtl(c[j]); j++) {}
        if (i > 0 && type[i - 1] && j < n && type[j])
            while (i < j) type[i++] = 1;
        i = j - 1;
    }
    for (i = 0, j = n - 1; i < j; i++, j--)
    {
        u32 t = c[i]; c[i] = c[j]; c[j] = t;
        u8 y = type[i]; type[i] = type[j]; type[j] = y;
    }
    for (i = 0; i < n; i++)
    {
        if (!type[i]) { c[i] = mirror(c[i]); continue; }
        for (j = i; j < n && type[j]; j++) {}
        {
            int a = i, b = j - 1;
            while (a < b) { u32 t = c[a]; c[a] = c[b]; c[b] = t; a++; b--; }
        }
        i = j - 1;
    }
}

static void draw_glyph(const UiAtlasGlyph *g, float *x, float baseline, float k, const float *p, D3DCOLOR color)
{
    if (g->w)
    {
        float gx = *x + g->left * k, gy = baseline + g->top * k;
        quad(gx, gy, gx + g->w * k, gy + g->h * k, (float)g->x / UI_ATLAS_W, (float)g->y / UI_ATLAS_H,
             (float)(g->x + g->w) / UI_ATLAS_W, (float)(g->y + g->h) / UI_ATLAS_H, 1.0f, 0.0f, p, color, color);
    }
    *x += g->advance * k;
}

// One line; `range` scales the edge sharpness (1: crisp), `bias` the edge offset.
static void draw_line(int font, float size, float x, float baseline, D3DCOLOR color, const char *s, int n, float range, float bias)
{
    float k = size / UI_ATLAS_BASE, p[4];
    const char *end = s + n;
    u32 cps[UI_BIDI_MAX];
    int count = 0, rtl = 0, i;
    p[0] = 2.0f * UI_ATLAS_SPREAD * k * range;
    // Small text: a slightly heavier edge keeps it from looking thin on a TV.
    p[1] = bias + (size < 24.0f ? (24.0f - size) * 0.03f : 0.0f);
    p[2] = 0.0f;
    p[3] = 1.0f;
    while (s < end)
    {
        u32 c = utf8_next(&s, end);
        rtl |= is_rtl(c);
        cps[count++] = c;
        if (count == UI_BIDI_MAX)
        {
            // A very long line: shown in pieces of UI_BIDI_MAX characters.
            if (rtl) ui_bidi(cps, count);
            for (i = 0; i < count; i++) draw_glyph(glyph(font, cps[i]), &x, baseline, k, p, color);
            count = rtl = 0;
        }
    }
    if (rtl) ui_bidi(cps, count);
    for (i = 0; i < count; i++) draw_glyph(glyph(font, cps[i]), &x, baseline, k, p, color);
}

static float text_impl(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align, float range, float bias)
{
    float best = 0, lineH = size * 1.35f, baseline = cy + s_atlasFont[font ? 1 : 0].cap * size / UI_ATLAS_BASE * 0.5f;
    for (;;)
    {
        const char *nl = strchr(text, '\n');
        int n = nl ? (int)(nl - text) : (int)strlen(text);
        float w = line_width(font, size, text, n), lx = align == UI_CENTER ? x - w * 0.5f : align == UI_RIGHT ? x - w : x;
        draw_line(font, size, lx, baseline, color, text, n, range, bias);
        if (w > best) best = w;
        if (!nl) break;
        text = nl + 1;
        baseline += lineH;
    }
    return best;
}

float UiText(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align)
{
    return text_impl(font, size, x, cy, color, text, align, 1.0f, 0.5f);
}

float UiTextShadowed(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align)
{
    text_impl(font, size, x, cy + size * 0.06f + 1.0f, with_alpha(D3DCOLOR_ARGB(170, 0, 0, 0), (float)(color >> 24) / 255.0f), text, align,
              0.3f, 0.55f);
    return text_impl(font, size, x, cy, color, text, align, 1.0f, 0.5f);
}

float UiTextWidth(int font, float size, const char *text)
{
    float best = 0;
    for (;;)
    {
        const char *nl = strchr(text, '\n');
        int n = nl ? (int)(nl - text) : (int)strlen(text);
        float w = line_width(font, size, text, n);
        if (w > best) best = w;
        if (!nl) break;
        text = nl + 1;
    }
    return best;
}

void UiTextFit(int font, float size, float maxW, const char *text, char *out, int outSize)
{
    const int ellBytes = (int)sizeof(UI_CH_ELLIPSIS) - 1;
    int n = (int)strlen(text), cut = 0;
    float ell = line_width(font, size, UI_CH_ELLIPSIS, ellBytes), w = 0, k = size / UI_ATLAS_BASE;
    const char *p = text, *end = text + n;
    if (outSize <= 0) return;
    if (line_width(font, size, text, n) <= maxW || n == 0)
    {
        strncpy(out, text, outSize - 1);
        out[outSize - 1] = 0;
        return;
    }
    // The longest start of whole characters that fits with the ellipsis, without a space at its end.
    while (p < end)
    {
        const char *q = p;
        w += glyph(font, utf8_next(&q, end))->advance * k;
        if (w + ell > maxW || (int)(q - text) > outSize - 1 - ellBytes) break;
        p = q;
        if (p[-1] != ' ') cut = (int)(p - text);
    }
    memcpy(out, text, cut);
    memcpy(out + cut, UI_CH_ELLIPSIS, ellBytes + 1);
}

int UiTextWrap(int font, float size, float maxW, int maxLines, const char *text, char *out, int outSize)
{
    int lines = 0, used = 0;
    const char *p = text;
    out[0] = 0;
    while (*p && lines < maxLines)
    {
        // The longest run of whole words that fits (a single long word is cut).
        const char *end = p, *lastFit = NULL;
        int n;
        while (*p == ' ') p++;
        end = p;
        for (;;)
        {
            const char *q = end;
            while (*q && *q != ' ' && *q != '\n') q++;
            if (line_width(font, size, p, (int)(q - p)) > maxW) break;
            lastFit = q;
            if (!*q || *q == '\n') break;
            end = q + 1;
        }
        if (!lastFit)
        {
            // Not even one word: cut it to the width.
            lastFit = p;
            while (*lastFit && *lastFit != ' ' && *lastFit != '\n' &&
                   line_width(font, size, p, (int)(lastFit - p) + utf8_len(lastFit)) <= maxW)
                lastFit += utf8_len(lastFit);
            if (lastFit == p) lastFit += utf8_len(lastFit);
        }
        n = (int)(lastFit - p);
        if (lines == maxLines - 1 && *lastFit && *lastFit != '\n' && lastFit[1])
        {
            // Last allowed line and text remains: cut what is left with an ellipsis.
            char tmp[256];
            int rest = (int)strlen(p);
            if (rest > (int)sizeof(tmp) - 1) rest = (int)sizeof(tmp) - 1;
            memcpy(tmp, p, rest);
            tmp[rest] = 0;
            {
                char *nl = strchr(tmp, '\n');
                if (nl) *nl = 0;
            }
            if (used + 2 >= outSize) break;
            if (lines) out[used++] = '\n';
            UiTextFit(font, size, maxW, tmp, out + used, outSize - used);
            used += (int)strlen(out + used);
            lines++;
            break;
        }
        if (used + n + 2 >= outSize) break;
        if (lines) out[used++] = '\n';
        memcpy(out + used, p, n);
        used += n;
        out[used] = 0;
        lines++;
        p = lastFit;
        if (*p == '\n') p++;
    }
    return lines;
}

// ---- Logo ----
float UiLogo(float x, float y, float size)
{
    float k = size / 128.0f, p[4], u0, v0, u1, v1;
    int i;
    static const D3DCOLOR tops[3] = { D3DCOLOR_ARGB(255, 255, 98, 72), D3DCOLOR_ARGB(110, 255, 236, 228), D3DCOLOR_ARGB(255, 120, 206, 92) };
    static const D3DCOLOR bottoms[3] = { D3DCOLOR_ARGB(255, 190, 30, 26), D3DCOLOR_ARGB(70, 255, 236, 228), D3DCOLOR_ARGB(255, 46, 142, 64) };
    p[2] = 0.0f;
    p[3] = 1.0f;
    for (i = -1; i < 3; i++)
    {
        const UiAtlasRect *r = &s_atlasLogo[i < 0 ? 0 : i];
        float dy = 0.0f;
        D3DCOLOR ct, cb;
        u0 = (float)r->x / UI_ATLAS_W; v0 = (float)r->y / UI_ATLAS_H;
        u1 = (float)(r->x + r->w) / UI_ATLAS_W; v1 = (float)(r->y + r->h) / UI_ATLAS_H;
        if (i < 0)
        {
            // A soft shadow of the body, a little lower.
            p[0] = 2.0f * UI_ATLAS_LOGO_SPREAD * k * 0.18f;
            p[1] = 0.6f;
            dy = size * 0.04f;
            ct = cb = D3DCOLOR_ARGB(120, 0, 0, 0);
        }
        else
        {
            p[0] = 2.0f * UI_ATLAS_LOGO_SPREAD * k;
            p[1] = 0.5f;
            ct = tops[i];
            cb = bottoms[i];
        }
        quad(x, y + dy, x + r->w * k, y + dy + r->h * k, u0, v0, u1, v1, 1.0f, 0.0f, p, ct, cb);
    }
    return size;
}

float UiBrand(float x, float cy, float size)
{
    float logo = size * 1.55f, tx = x + logo + size * 0.3f, w, bx, bw, bh = size * 0.62f;
    UiLogo(x, cy - logo * 0.52f, logo);
    w = UiText(UI_BOLD, size, tx, cy, UI_COL_TEXT, "Harissa64", UI_LEFT);
    bx = tx + w + size * 0.3f;
    bw = UiTextWidth(UI_BOLD, size * 0.42f, "V2") + size * 0.5f;
    UiRoundRect(bx, cy - bh * 0.5f, bw, bh, bh * 0.32f, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    UiText(UI_BOLD, size * 0.42f, bx + bw * 0.5f, cy, UI_COL_TEXT, "V2", UI_CENTER);
    return bx + bw - x;
}

// ---- Button prompts ----
#define BTN_R 15.0f
#define PILL_H 26.0f
#define HINT_SIZE 19.0f

// Draws (or only measures when draw is 0) one button glyph; returns its width.
static float button_glyph(float x, float cy, const char *name, int draw)
{
    D3DCOLOR fill = 0, fill2 = 0, letter = UI_COL_TEXT;
    if (!strcmp(name, "A"))      { fill = D3DCOLOR_ARGB(255, 116, 196, 72); fill2 = D3DCOLOR_ARGB(255, 70, 150, 40); }
    else if (!strcmp(name, "B")) { fill = D3DCOLOR_ARGB(255, 246, 92, 74);  fill2 = D3DCOLOR_ARGB(255, 200, 40, 32); }
    else if (!strcmp(name, "X")) { fill = D3DCOLOR_ARGB(255, 70, 146, 240); fill2 = D3DCOLOR_ARGB(255, 30, 98, 200); }
    else if (!strcmp(name, "Y")) { fill = D3DCOLOR_ARGB(255, 252, 206, 66); fill2 = D3DCOLOR_ARGB(255, 222, 160, 16); letter = D3DCOLOR_ARGB(255, 40, 32, 10); }
    if (fill)
    {
        if (draw)
        {
            UiShadow(x, cy - BTN_R + 2, BTN_R * 2, BTN_R * 2, BTN_R, 6, D3DCOLOR_ARGB(110, 0, 0, 0));
            UiRoundRect(x, cy - BTN_R, BTN_R * 2, BTN_R * 2, BTN_R, fill, fill2);
            UiText(UI_BOLD, 17, x + BTN_R, cy, letter, name, UI_CENTER);
        }
        return BTN_R * 2;
    }
    if (!strcmp(name, "DPAD"))
    {
        if (draw)
        {
            float cx = x + BTN_R;
            D3DCOLOR c = D3DCOLOR_ARGB(255, 214, 217, 226);
            UiRoundRect(cx - 14, cy - 5, 28, 10, 3, c, c);
            UiRoundRect(cx - 5, cy - 14, 10, 28, 3, c, c);
            UiCircle(cx, cy, 2.5f, D3DCOLOR_ARGB(255, 90, 94, 108));
        }
        return BTN_R * 2;
    }
    {
        // Bumpers, BACK, START: an outlined pill with the name.
        float w = UiTextWidth(UI_BOLD, 14, name) + 22;
        if (w < PILL_H + 8) w = PILL_H + 8;
        if (draw)
        {
            UiRoundRect(x, cy - PILL_H * 0.5f, w, PILL_H, PILL_H * 0.5f, D3DCOLOR_ARGB(255, 58, 62, 76), D3DCOLOR_ARGB(255, 44, 47, 58));
            UiRoundRing(x, cy - PILL_H * 0.5f, w, PILL_H, PILL_H * 0.5f, 1.0f, D3DCOLOR_ARGB(60, 255, 255, 255));
            UiText(UI_BOLD, 14, x + w * 0.5f, cy, UI_COL_TEXT, name, UI_CENTER);
        }
        return w;
    }
}

static float footer_impl(float x, float cy, const char *hints, int draw)
{
    char buf[256], *item, *nextItem;
    float x0 = x;
    int first = 1;
    strncpy(buf, hints, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (item = buf; item && *item; item = nextItem)
    {
        char *label, *btn, *nextBtn;
        nextItem = strchr(item, '|');
        if (nextItem) *nextItem++ = 0;
        label = strchr(item, ':');
        if (label) *label++ = 0;
        if (!first) x += 30;
        first = 0;
        for (btn = item; btn && *btn; btn = nextBtn)   // "LB/RB": glyphs sharing one label
        {
            nextBtn = strchr(btn, '/');
            if (nextBtn) *nextBtn++ = 0;
            x += button_glyph(x, cy, btn, draw);
            if (nextBtn && *nextBtn) x += 6;
        }
        if (label)
        {
            x += 10;
            if (draw) UiText(UI_REGULAR, HINT_SIZE, x, cy, UI_COL_TEXT2, label, UI_LEFT);
            x += UiTextWidth(UI_REGULAR, HINT_SIZE, label);
        }
    }
    return x - x0;
}

float UiFooter(float x, float cy, const char *hints) { return footer_impl(x, cy, hints, 1); }
float UiFooterWidth(const char *hints) { return footer_impl(0, 0, hints, 0); }

void UiScreenFooter(const char *hints)
{
    UiRect(UI_SAFE_X, 636, UI_WIDTH - 2 * UI_SAFE_X, 1, UI_COL_LINE);
    UiFooter(UI_SAFE_X, 664, hints);
}

// ---- Notifications ----
void UiToast(const char *text, float age, float life)
{
    float in = age / 200.0f, out = (life - age) / 350.0f, a, slide, w, h = 50, x, y;
    if (age < 0 || age > life) return;
    if (in > 1) in = 1;
    if (out > 1) out = 1;
    if (out < 0) out = 0;
    a = in < out ? in : out;
    slide = (1.0f - in) * (1.0f - in) * 18.0f;
    w = UiTextWidth(UI_REGULAR, 21, text) + 72;
    x = (UI_WIDTH - w) * 0.5f;
    y = UI_HEIGHT - UI_SAFE_Y - 40 - h + slide;
    UiShadow(x, y + 6, w, h, h * 0.5f, 22, with_alpha(D3DCOLOR_ARGB(150, 0, 0, 0), a));
    UiRoundRect(x, y, w, h, h * 0.5f, with_alpha(D3DCOLOR_ARGB(240, 40, 42, 54), a), with_alpha(D3DCOLOR_ARGB(240, 30, 32, 42), a));
    UiRoundRing(x, y, w, h, h * 0.5f, 1.0f, with_alpha(UI_COL_LINE, a));
    UiCircle(x + 28, y + h * 0.5f, 6, with_alpha(UI_COL_ACCENT, a));
    UiText(UI_REGULAR, 21, x + 46, y + h * 0.5f, with_alpha(UI_COL_TEXT, a), text, UI_LEFT);
}

void UiFpsBadge(const char *text)
{
    float w = UiTextWidth(UI_BOLD, 17, text) + 26, h = 32, x = UI_WIDTH - UI_SAFE_X - w, y = UI_SAFE_Y;
    UiRoundRect(x, y, w, h, h * 0.5f, D3DCOLOR_ARGB(190, 24, 26, 34), D3DCOLOR_ARGB(190, 16, 17, 23));
    UiRoundRing(x, y, w, h, h * 0.5f, 1.0f, UI_COL_LINE);
    UiText(UI_BOLD, 17, x + w * 0.5f, y + h * 0.5f, UI_COL_TEXT, text, UI_CENTER);
}

void UiPerfPanel(const char *text)
{
    int lines = 1;
    const char *p;
    float w, h, x, y;
    for (p = text; *p; p++) lines += *p == '\n';
    w = UiTextWidth(UI_BOLD, 17, text) + 30;
    h = 14 + lines * 17 * 1.35f;
    x = UI_WIDTH - UI_SAFE_X - w;
    y = UI_SAFE_Y;
    UiRoundRect(x, y, w, h, 14, D3DCOLOR_ARGB(190, 24, 26, 34), D3DCOLOR_ARGB(190, 16, 17, 23));
    UiRoundRing(x, y, w, h, 14, 1.0f, UI_COL_LINE);
    UiText(UI_BOLD, 17, x + 15, y + 7 + 17 * 1.35f * 0.5f, UI_COL_TEXT, text, UI_LEFT);
}

// ---- Menu input ----
// A held direction or shoulder button repeats (LB/RB page through the ROM
// list as if pressed again and again), faster after REPEAT_FAST_AFTER_MS.
#define REPEAT_FIRST_MS 350
#define REPEAT_NEXT_MS 90
#define REPEAT_FAST_MS 45
#define REPEAT_FAST_AFTER_MS 1000
static const WORD REPEATS = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT |
                            XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;

void UiInputInit(UiInput *in)
{
    XINPUT_STATE st;
    in->held = in->prev = 0;
    in->repeatAt = in->holdStart = 0;
    // Buttons already down (the press that opened the menu) do not count.
    if (XInputGetState(0, &st) == ERROR_SUCCESS) in->prev = st.Gamepad.wButtons;
}

WORD UiInputPoll(UiInput *in)
{
    XINPUT_STATE st;
    WORD b = 0, down;
    DWORD now = GetTickCount();
    if (XInputGetState(0, &st) == ERROR_SUCCESS)
    {
        b = st.Gamepad.wButtons;
        if (st.Gamepad.sThumbLY > 20000) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -20000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX < -20000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (st.Gamepad.sThumbLX > 20000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }
    down = b & ~in->prev;
    if (down & REPEATS)
    {
        in->repeatAt = now + REPEAT_FIRST_MS;
        in->holdStart = now;
    }
    else if ((b & REPEATS) && (s32)(now - in->repeatAt) >= 0)
    {
        down |= b & REPEATS;
        in->repeatAt = now + (now - in->holdStart >= REPEAT_FAST_AFTER_MS ? REPEAT_FAST_MS : REPEAT_NEXT_MS);
    }
    in->prev = b;
    in->held = b;
    return down;
}
