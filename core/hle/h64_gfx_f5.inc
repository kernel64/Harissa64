// Harissa64 V2 - graphics HLE: Factor 5's microcodes (included by h64_gfx.cpp).
//
// Star Wars: Rogue Squadron (F3DSWRS). Studied in GLideN64's uCodes/F5Rogue.cpp
// (GPL v2: Lemmy's LemNemu port, microcode decoding by olivieryuyu) and
// rewritten for V2's vertices and RDP output. The display lists are F3D-like
// with Factor 5's commands: vertices are 8 bytes (x, y, z, flag) without
// colour or texture coordinates, triangles carry colour indices into a
// colour table and their own s/t, a "jump" command follows a link word kept
// at the start of each display list, terrain patches are generated from
// height maps (TRI_GEN) and sprites are placed from one vertex (TEXRECT_GEN).
// GLideN64 keeps RDRAM as host-order 32-bit words: its byte offsets x map to
// N64 bytes x ^ 3, which the code below follows where a table is read by bytes.

// The current command's address (the parse loop has moved pc past it).
static H64_INLINE u32 f5_cur(const H64Gfx *g) { return g->pc[g->pci] - 8; }

// A display list entered: its first word is the link a later G_JUMPSWDL
// follows; the commands start after the first 8 bytes (GLideN64 _updateF5DL).
static void f5_enter(H64Gfx *g)
{
    u32 a = g->pc[g->pci];
    g->f5dl[g->pci] = rd32(g, a) & 0x00FFFFFF;
    g->pc[g->pci] = a + 8;
}

// Vertex i at model position (x, y, z), transformed and projected as G_VTX
// does (no lighting: the colours come with the triangles).
static void f5_vertex(H64Gfx *g, u32 i, s16 px, s16 py, s16 pz)
{
    GfxVtx *v;
    const float (*m)[4];
    float x, y, z, w, cr = g->clipRatio;
    u32 clip = 0;
    if (i >= GFX_VTX_MAX) return;
    update_combined(g);
    m = g->f5Mtx ? (const float (*)[4])g->f5Mtx : g->combined;   // Indiana Jones' vertex generator matrix
    v = &g->vtx[i];
    x = px * m[0][0] + py * m[1][0] + pz * m[2][0] + m[3][0];
    y = px * m[0][1] + py * m[1][1] + pz * m[2][1] + m[3][1];
    z = px * m[0][2] + py * m[1][2] + pz * m[2][2] + m[3][2];
    w = px * m[0][3] + py * m[1][3] + pz * m[2][3] + m[3][3];
    y = -y;   // the microcode's y points the other way (GLideN64 gSPLoadSWVertexData)
    if (x > w) clip |= CL_POSX;
    if (x < -w) clip |= CL_NEGX;
    if (y > w) clip |= CL_POSY;
    if (y < -w) clip |= CL_NEGY;
    if (w < 0.01f) clip |= CL_W;
    if (x > cr * w || x < -cr * w || y > cr * w || y < -cr * w) clip |= CL_GUARD;
    v->x = x; v->y = y; v->z = z; v->w = w;
    v->nx = v->ny = v->nz = 0;
    v->clip = clip;
    v->xyOverride = v->zOverride = v->stOverride = 0;
    v->projGen = 0;
    if ((geom(g) & GM_FOG) && w > 0)
    {
        float f = z / w * g->fogMul + g->fogOff;
        v->a = f < 0 ? 0.0f : f > 255.0f ? 255.0f : f;
    }
    if (w >= 0.0001f)
    {
        float iw = 1.0f / w;
        float sx = g->vtrans[0] + g->vscale[0] * x * iw, sy = g->vtrans[1] - g->vscale[1] * y * iw;
        v->px = sx;
        v->py = sy;
        v->pz = (g->vtrans[2] + g->vscale[2] * z * iw) * 32.0f;
        v->iw = iw;
        if (sx >= -1000.0f && sx <= 2000.0f && sy >= -1000.0f && sy <= 2000.0f) v->projGen = g->vpGen;
    }
    g->nVerts++;
}

// Colour (r, g, b, a in the word's bytes from the top) and texture
// coordinates of vertex i (GLideN64 F3DSWRS_PrepareVertices): s and t are
// scaled by the two halves of the G_MV_TEXSCALE words as the vector unit does
// (low product's upper half + high product's lower half), and come in s10.5
// with perspective correction, s10.4 without (the RDP then reads them whole).
static void f5_prep(H64Gfx *g, u32 i, u32 rgba, int useTex, u32 st, int persp)
{
    GfxVtx *v;
    if (i >= GFX_VTX_MAX) return;
    v = &g->vtx[i];
    v->r = s_u8f[rgba >> 24];
    v->g = s_u8f[(rgba >> 16) & 0xFF];
    v->b = s_u8f[(rgba >> 8) & 0xFF];
    if (!(geom(g) & GM_FOG)) v->a = s_u8f[rgba & 0xFF];
    if (useTex)
    {
        u32 s = (u32)(s32)(s16)(st >> 16), t = (u32)(s32)(s16)st;
        u32 sc0 = g->f5TexScale[0], sc1 = g->f5TexScale[1];
        s16 ss = (s16)((s16)((s * (sc0 >> 16)) >> 16) + (s16)(s * (sc1 >> 16)));
        s16 tt = (s16)((s16)((t * (sc0 & 0xFFFF)) >> 16) + (s16)(t * (sc1 & 0xFFFF)));
        v->s = persp ? (float)ss : (float)ss * 2.0f;
        v->t = persp ? (float)tt : (float)tt * 2.0f;
    }
}

// A colour of the table at byte offset idx, read as GLideN64 reads it.
static u32 f5_color_at(H64Gfx *g, u32 addr)
{
    return (u32)rd8(g, (addr + 3) ^ 3) << 24 | (u32)rd8(g, (addr + 2) ^ 3) << 16 |
           (u32)rd8(g, (addr + 1) ^ 3) << 8 | rd8(g, addr ^ 3);
}

// ---- TRI_GEN: terrain patches from height maps.
struct F5Tri { u32 v0, v1, v2; int z; };

static const u8 s_f5order0[32][3] = {
    { 0, 6, 1 }, { 0, 5, 6 }, { 1, 7, 2 }, { 1, 6, 7 }, { 2, 8, 3 }, { 5, 11, 6 }, { 2, 7, 8 }, { 5, 10, 11 },
    { 3, 9, 4 }, { 3, 8, 9 }, { 6, 12, 7 }, { 6, 11, 12 }, { 10, 16, 11 }, { 7, 13, 8 }, { 10, 15, 16 },
    { 7, 12, 13 }, { 8, 14, 9 }, { 11, 17, 12 }, { 8, 13, 14 }, { 11, 16, 17 }, { 15, 21, 16 }, { 12, 18, 13 },
    { 15, 20, 21 }, { 12, 17, 18 }, { 13, 19, 14 }, { 16, 22, 17 }, { 16, 21, 22 }, { 13, 18, 19 },
    { 17, 23, 18 }, { 17, 22, 23 }, { 18, 24, 19 }, { 18, 23, 24 } };
static const u8 s_f5order1[8][3] = {
    { 0, 3, 4 }, { 0, 4, 1 }, { 1, 4, 5 }, { 1, 5, 2 }, { 3, 6, 7 }, { 3, 7, 4 }, { 4, 7, 8 }, { 4, 8, 5 } };

// Heights are kept in GLideN64's order (entry k at k ^ 1), colours as words.
struct F5Patch
{
    u16 h[2048];
    u32 col[512];
    u32 ncol;
    F5Tri tri[48];
    u32 ntri;
};

static void f5_avg_h(F5Patch *p, u32 a, u32 b, u32 d)
{
    p->h[d] = (u16)(((s16)p->h[a] + (s16)p->h[b]) / 2);
}

static void f5_avg_c(F5Patch *p, u32 a, u32 b, u32 d)
{
    u32 k, r = 0;
    for (k = 0; k < 32; k += 8) r |= ((((p->col[a] >> k) & 0xFF) + ((p->col[b] >> k) & 0xFF)) >> 1) << k;
    p->col[d] = r;
}

static void f5_pond_h(F5Patch *p, u32 a, u32 b, u32 d, u16 pw)
{
    s32 vd = (s32)(s16)p->h[d] * (s32)(0x10000 - pw);
    vd += (((s32)(s16)p->h[a] + (s32)(s16)p->h[b]) * (s32)pw) >> 1;
    p->h[d] = (u16)((u32)vd >> 16);
}

static void f5_pond_c(F5Patch *p, u32 a, u32 b, u32 d, u16 pw)
{
    u32 k, r = 0;
    for (k = 0; k < 32; k += 8)
    {
        u32 c = ((p->col[d] >> k) & 0xFF) * (0x10000 - pw) + (((((p->col[a] >> k) & 0xFF) + ((p->col[b] >> k) & 0xFF)) * pw) >> 1);
        r |= ((c >> 16) & 0xFF) << k;
    }
    p->col[d] = r;
}

static void f5_add_tri(F5Patch *p, u32 a, u32 b, u32 c)
{
    if (p->ntri >= 48) return;
    p->tri[p->ntri].v0 = a; p->tri[p->ntri].v1 = b; p->tri[p->ntri].v2 = c;
    p->ntri++;
}

// Vertex k = i * step + j of a step x step patch: x from params 8/9 plus i
// steps, z plus j steps, y the height (all 16-bit lanes, wrapping).
static void f5_patch_vertices(H64Gfx *g, const u32 *prm, u32 step, const F5Patch *p)
{
    u16 x0 = (u16)(prm[8] >> 16), y0 = (u16)((prm[8] & 0xFFFF) << 4), z0 = (u16)(prm[9] >> 16), d = (u16)prm[9];
    u32 i, j, k = 0;
    for (i = 0; i < step; i++)
        for (j = 0; j < step; j++, k++)
            f5_vertex(g, k, (s16)(u16)(x0 + i * d), (s16)(u16)(y0 + p->h[(i + j * step) ^ 1]), (s16)(u16)(z0 + j * d));
}

static void f5_patch_colors(H64Gfx *g, const u32 *prm, u32 step, const F5Patch *p)
{
    u16 tex = (u16)prm[7];
    u32 i, j;
    for (i = 0; i < step; i++)
        for (j = 0; j < step; j++)
        {
            u32 c = i + j * step;
            u32 st = (u32)(u16)(tex * i) << 16 | (u16)(tex * (step - 1 - j));
            f5_prep(g, i * step + j, c < p->ncol ? p->col[c] : 0, 1, st, 0);
        }
}

static int f5_screen_z(const H64Gfx *g, u32 i)
{
    const GfxVtx *v = &g->vtx[i];
    return (int)((v->z / v->w * g->vscale[2] + g->vtrans[2]) * 32.0f);
}

// Sorted far to near (stable), drawn as G_TRI1 would.
static void f5_draw_patch(H64Gfx *g, F5Patch *p, const u8 (*order)[3], u32 norder)
{
    u32 i, j;
    for (i = 0; i < norder; i++) f5_add_tri(p, order[i][0], order[i][1], order[i][2]);
    for (i = 0; i < p->ntri; i++)
        p->tri[i].z = (f5_screen_z(g, p->tri[i].v0) + f5_screen_z(g, p->tri[i].v1) + f5_screen_z(g, p->tri[i].v2)) / 3;
    for (i = 1; i < p->ntri; i++)
    {
        F5Tri t = p->tri[i];
        for (j = i; j > 0 && p->tri[j - 1].z < t.z; j--) p->tri[j] = p->tri[j - 1];
        p->tri[j] = t;
    }
    for (i = 0; i < p->ntri; i++) triangle(g, p->tri[i].v0, p->tri[i].v1, p->tri[i].v2);
}

static void f5_trigen_0000(H64Gfx *g, const u32 *prm, F5Patch *p)
{
    u32 a = prm[3] & 0xFFFFFF, i, idx8 = prm[2] & 7, base = prm[2] & 0xFFFFF8;
    u16 pw;
    for (i = 0; i < 25; i++) p->col[i] = rd32(g, a + i * 4);
    p->ncol = 25;
    memset(p->h, 0, 26 * sizeof(u16));
    for (i = 0; i < 25; i++) p->h[i ^ 1] = (u16)(s16)((s8)rd8(g, base + idx8 + i) * 16);
    if ((prm[1] & 0xFF00) == 0x0100)
    {
        f5_avg_h(p, 1, 3, 0); f5_avg_h(p, 3, 5, 2);
        f5_avg_c(p, 0, 2, 1); f5_avg_c(p, 2, 4, 3);
        f5_add_tri(p, 0, 10, 5); f5_add_tri(p, 10, 20, 15);
    }
    if ((pw = (u16)prm[6]) != 0)
    {
        f5_pond_h(p, 1, 3, 0, pw); f5_pond_h(p, 3, 5, 2, pw);
        f5_pond_c(p, 0, 2, 1, pw); f5_pond_c(p, 2, 4, 3, pw);
    }
    if ((prm[1] & 0xFF) == 0x01)
    {
        f5_avg_h(p, 21, 23, 20); f5_avg_h(p, 23, 25, 22);
        f5_avg_c(p, 20, 22, 21); f5_avg_c(p, 22, 24, 23);
        f5_add_tri(p, 4, 9, 14); f5_add_tri(p, 14, 19, 24);
    }
    if ((pw = (u16)(prm[7] >> 16)) != 0)
    {
        f5_pond_h(p, 21, 23, 20, pw); f5_pond_h(p, 23, 25, 22, pw);
        f5_pond_c(p, 20, 22, 21, pw); f5_pond_c(p, 22, 24, 23, pw);
    }
    if ((prm[1] & 0xFF000000) == 0x01000000)
    {
        f5_avg_h(p, 1, 11, 4); f5_avg_h(p, 11, 21, 14);
        f5_avg_c(p, 0, 10, 5); f5_avg_c(p, 10, 20, 15);
        f5_add_tri(p, 0, 1, 2); f5_add_tri(p, 2, 3, 4);
    }
    if ((pw = (u16)prm[5]) != 0)
    {
        f5_pond_h(p, 1, 11, 4, pw); f5_pond_h(p, 11, 21, 14, pw);
        f5_pond_c(p, 0, 10, 5, pw); f5_pond_c(p, 10, 20, 15, pw);
    }
    if ((prm[1] & 0xFF0000) == 0x010000)
    {
        f5_avg_h(p, 5, 15, 8); f5_avg_h(p, 15, 25, 18);
        f5_avg_c(p, 4, 14, 9); f5_avg_c(p, 14, 24, 19);
        f5_add_tri(p, 22, 24, 23); f5_add_tri(p, 20, 22, 21);
    }
    if ((pw = (u16)(prm[6] >> 16)) != 0)
    {
        f5_pond_h(p, 5, 15, 8, pw); f5_pond_h(p, 15, 25, 18, pw);
        f5_pond_c(p, 4, 14, 9, pw); f5_pond_c(p, 14, 24, 19, pw);
    }
    if ((pw = (u16)(prm[5] >> 16)) != 0)
    {
        static const u8 hv[8][3] = { { 1, 13, 7 }, { 3, 13, 6 }, { 3, 15, 9 }, { 11, 13, 10 }, { 13, 15, 12 },
                                     { 11, 23, 17 }, { 13, 23, 16 }, { 13, 25, 19 } };
        static const u8 cv[8][3] = { { 0, 12, 6 }, { 2, 12, 7 }, { 2, 14, 8 }, { 10, 12, 11 }, { 12, 14, 13 },
                                     { 10, 22, 16 }, { 12, 22, 17 }, { 12, 24, 18 } };
        for (i = 0; i < 8; i++) f5_pond_h(p, hv[i][0], hv[i][1], hv[i][2], pw);
        for (i = 0; i < 8; i++) f5_pond_c(p, cv[i][0], cv[i][1], cv[i][2], pw);
    }
    f5_patch_vertices(g, prm, 5, p);
    f5_patch_colors(g, prm, 5, p);
    f5_draw_patch(g, p, s_f5order0, 32);
}

static void f5_trigen_0001(H64Gfx *g, const u32 *prm, F5Patch *p)
{
    u32 dataEnd = ((prm[4] & 0xFFFF) + 0x13) & 0xFF0, off = prm[3] & 7, i;
    u32 vsize = ((prm[4] >> 16) + 0x16) & 0xFF0, base = prm[2] & 0xFFFFF8, idx8 = prm[2] & 7, n = 0;
    u16 pw;
    p->ncol = 0;
    if (dataEnd >= off)
    {
        u32 words = (dataEnd - off) / 4, a = (prm[3] & 0xFFFFF8) + off, k = 0;
        do
        {
            if (p->ncol + 3 > 512) break;
            p->col[p->ncol++] = rd32(g, a + k * 4); k += 2;
            p->col[p->ncol++] = rd32(g, a + k * 4); k += 2;
            p->col[p->ncol++] = rd32(g, a + k * 4); k += 6;
        } while (words > k);
    }
    while (p->ncol < 9) p->col[p->ncol++] = 0;
    memset(p->h, 0, sizeof(p->h));
    do
    {
        for (i = 0; i < 3 && n < 2047; i++)
        {
            p->h[n ^ 1] = (u16)(s16)((s8)rd8(g, base + idx8) * 16);
            idx8 += 2;
            n++;
        }
        idx8 += 4;
    } while (vsize > idx8 && n < 2047);
    if ((prm[1] & 0xFF00) == 0x0200) { f5_avg_h(p, 1, 3, 0); f5_avg_c(p, 0, 2, 1); f5_add_tri(p, 0, 6, 3); }
    if ((pw = (u16)prm[6]) != 0) { f5_pond_h(p, 1, 3, 0, pw); f5_pond_c(p, 0, 2, 1, pw); }
    if ((prm[1] & 0xFF) == 0x02) { f5_avg_h(p, 7, 9, 8); f5_avg_c(p, 6, 8, 7); f5_add_tri(p, 2, 5, 8); }
    if ((pw = (u16)(prm[7] >> 16)) != 0) { f5_pond_h(p, 7, 9, 6, pw); f5_pond_c(p, 6, 8, 7, pw); }
    if ((prm[1] & 0xFF000000) == 0x02000000) { f5_avg_h(p, 1, 7, 2); f5_avg_c(p, 0, 6, 3); f5_add_tri(p, 0, 1, 2); }
    if ((pw = (u16)prm[5]) != 0) { f5_pond_h(p, 1, 7, 2, pw); f5_pond_c(p, 0, 6, 3, pw); }
    if ((prm[1] & 0x00FF0000) == 0x00020000) { f5_avg_h(p, 3, 9, 4); f5_avg_c(p, 2, 8, 5); f5_add_tri(p, 6, 8, 7); }
    if ((pw = (u16)(prm[6] >> 16)) != 0) { f5_pond_h(p, 3, 9, 4, pw); f5_pond_c(p, 2, 8, 5, pw); }
    if ((pw = (u16)(prm[5] >> 16)) != 0) { f5_pond_h(p, 1, 9, 5, pw); f5_pond_c(p, 0, 8, 4, pw); }
    f5_patch_vertices(g, prm, 3, p);
    f5_patch_colors(g, prm, 3, p);
    f5_draw_patch(g, p, s_f5order1, 8);
}

// A single quad: four corners, colours from the command.
static void f5_trigen_02(H64Gfx *g, const u32 *prm)
{
    u16 x0 = (u16)(prm[8] >> 16), y0 = (u16)((prm[8] & 0xFFFF) << 4), z0 = (u16)(prm[9] >> 16), d = (u16)prm[9];
    u32 tex = prm[7] & 0xFFFF;
    f5_vertex(g, 0, (s16)x0, (s16)(u16)(y0 + (prm[1] >> 16)), (s16)z0);
    f5_vertex(g, 1, (s16)(u16)(x0 + d), (s16)(u16)(y0 + (prm[1] & 0xFFFF)), (s16)z0);
    f5_vertex(g, 2, (s16)x0, (s16)(u16)(y0 + (prm[2] >> 16)), (s16)(u16)(z0 + d));
    f5_vertex(g, 3, (s16)(u16)(x0 + d), (s16)(u16)(y0 + (prm[2] & 0xFFFF)), (s16)(u16)(z0 + d));
    f5_prep(g, 0, prm[3], 1, tex, 0);
    f5_prep(g, 1, prm[4], 1, tex | (tex << 16), 0);
    f5_prep(g, 2, prm[5], 1, 0, 0);
    f5_prep(g, 3, prm[6], 1, tex << 16, 0);
    if (g->vtx[2].z / g->vtx[2].w > g->vtx[1].z / g->vtx[1].w)
    {
        triangle(g, 0, 1, 3);
        triangle(g, 0, 3, 2);
    }
    else
    {
        triangle(g, 0, 3, 2);
        triangle(g, 0, 1, 3);
    }
}

static void f5_trigen(H64Gfx *g, u32 w0)
{
    static F5Patch s_patch;   // parsing runs on one thread at a time
    u32 prm[10], i, cur = f5_cur(g);
    for (i = 0; i < 10; i++) prm[i] = rd32(g, cur + i * 4);
    s_patch.ntri = 0;
    switch ((w0 >> 8) & 0xFF)
    {
    case 0x00:
        if ((w0 & 0xFF) == 0x00) f5_trigen_0000(g, prm, &s_patch);
        else if ((w0 & 0xFF) == 0x01) f5_trigen_0001(g, prm, &s_patch);
        break;
    case 0x02: f5_trigen_02(g, prm); break;
    }
    g->pc[g->pci] += 32;
}

// G_TRI1 / G_TRI2 (two triangles sharing an edge): vertex numbers * 40, a word
// of colour indices, then s/t words when textured.
static void f5_tri(H64Gfx *g, u32 w0, u32 w1, int two)
{
    u32 cur = f5_cur(g), v[4], cp = rd32(g, cur + 8), idx[4], i, n = two ? 4 : 3;
    u32 gm = g->geomRaw, flat = ((w1 >> 24) & 3) % 3;
    int useTex = (w0 & 2) != 0, persp = (g->omH & 0x80000) != 0;
    v[0] = ((w1 >> 13) & 0x7F8) / 40;
    v[1] = ((w1 >> 5) & 0x7F8) / 40;
    v[2] = ((w1 << 3) & 0x7F8) / 40;
    v[3] = ((w1 >> 21) & 0x7F8) / 40;
    idx[0] = (cp >> 16) & 0xFF; idx[1] = (cp >> 8) & 0xFF; idx[2] = cp & 0xFF; idx[3] = cp >> 24;
    for (i = 0; i < n; i++)
        f5_prep(g, v[i], f5_color_at(g, g->f5ColorBase + idx[i]), useTex, useTex ? rd32(g, cur + 16 + i * 4) : 0, persp);
    if (useTex) g->pc[g->pci] += 16;
    g->pc[g->pci] += 8;
    // Culling both faces culls the back faces only here (GLideN64).
    if ((g->geomRaw & 0x3000) == 0x3000) g->geomRaw &= ~0x1000u;
    {
        u32 t[3];
        t[0] = v[0]; t[1] = v[1]; t[2] = v[2];
        triangle(g, t[flat], t[(flat + 1) % 3], t[(flat + 2) % 3]);
        if (two)
        {
            t[0] = v[0]; t[1] = v[2]; t[2] = v[3];
            triangle(g, t[flat], t[(flat + 1) % 3], t[(flat + 2) % 3]);
        }
    }
    g->geomRaw = gm;
}

// A rectangle centred on a vertex, its size from the projection
// (GLideN64 F3DSWRS_TexrectGen, kept in the same integer steps).
static void f5_texrect_gen(H64Gfx *g, u32 w0)
{
    u32 cur = f5_cur(g), vi = ((w0 >> 5) & 0x07F8) / 40, prm[5], i;
    const GfxVtx *v;
    float sx, sy, ulx, lrx, uly, lry, intpart;
    u32 wi, vp, pmX, pmY, offX, offY, p4X, p4Y, fx, fy, primZ;
    u64 p3X, p3Y;
    u16 dsdx, dtdy, sdsdx, sdtdy, E = 0, F = 0, S, T;
    int flip = (w0 & 1) != 0;
    u64 rect[2];
    for (i = 0; i < 5; i++) prm[i] = rd32(g, cur + i * 4);
    g->pc[g->pci] += 16;
    if (vi >= GFX_VTX_MAX) return;
    v = &g->vtx[vi];
    if (v->clip & (CL_POSX | CL_NEGX | CL_POSY | CL_NEGY | CL_W)) return;
    sx = g->vtrans[0] + g->vscale[0] * v->x / v->w;
    sy = g->vtrans[1] - g->vscale[1] * v->y / v->w;
    wi = v->w >= 1.0f ? (u32)v->w : 1;
    vp = rd32(g, g->f5VpAddr);
    pmX = (rd32(g, g->f5PerspAddr) & 0xFFFF0000u) | (rd32(g, g->f5PerspAddr + 32) >> 16);
    pmY = (rd32(g, g->f5PerspAddr + 8) << 16) | (rd32(g, g->f5PerspAddr + 40) & 0xFFFF);
    p3X = prm[3] >> 16;
    p3Y = prm[3] & 0xFFFF;
    if (flip) { u64 k = p3X; p3X = p3Y; p3Y = k; }
    offX = (u32)(((p3X * ((vp >> 17) & 0x7FFF) * pmX) / wi) >> 16);
    offY = (u32)(((p3Y * ((vp >> 1) & 0x7FFF) * pmY) / wi) >> 16);
    ulx = sx - offX / 4.0f;
    lrx = sx + offX / 4.0f;
    uly = sy - offY / 4.0f;
    lry = sy + offY / 4.0f;
    if (!(lrx - ulx > 0.0f) || !(lry - uly > 0.0f)) return;
    update_combined(g);
    fx = (u32)(fabsf(modff(g->combined[0][0], &intpart)) * 65536.0f);
    fy = (u32)(fabsf(modff(g->combined[0][1], &intpart)) * 65536.0f);
    p4X = (prm[4] & 0xFFFF0000u) | fx;
    p4Y = (prm[4] << 16) | fy;
    if (flip) { u32 k = offX; offX = offY; offY = k; }
    if (!offX || !offY) return;
    dsdx = (u16)((p4X / offX) >> 10);
    dtdy = (u16)((p4Y / offY) >> 10);
    if (w0 & 2) { dsdx = (u16)-dsdx; E = (u16)(prm[4] >> 16); }
    if (w0 & 4) { dtdy = (u16)-dtdy; F = (u16)prm[4]; }
    sdsdx = dsdx; sdtdy = dtdy;   // the rectangle's steps, before the flip swaps them for S and T
    if (flip) { u16 k = dsdx; dsdx = dtdy; dtdy = k; }
    S = ulx > 0 ? (u16)(E + 0xFFF0) : (u16)(((0 - ((int)dsdx << 6) * (int)(ulx * 4.0f)) << 3) + E + 0xFFF0);
    T = uly > 0 ? (u16)(F + 0xFFF0) : (u16)(((0 - ((int)dtdy << 6) * (int)(uly * 4.0f)) << 3) + F + 0xFFF0);
    {
        float z = (g->vtrans[2] + g->vscale[2] * v->z / v->w) * 32.0f;
        primZ = z < 0 ? 0 : z > 32767.0f ? 32767 : (u32)z;
    }
    out_rdp1(g, 0xEE000000u, primZ << 16);     // SET_PRIM_DEPTH
    out_rdp1(g, 0xFA000000u, prm[1]);          // SET_PRIM_COLOR
    if (geom(g) & GM_FOG)
        out_rdp1(g, 0xF8000000u, (prm[1] & 0xFFFFFF00u) | (u32)v->a);   // SET_FOG_COLOR, the vertex's fog in alpha
    {
        u32 xh = ulx < 0 ? 0 : (u32)(ulx * 4.0f), yh = uly < 0 ? 0 : (u32)(uly * 4.0f);
        u32 xl = lrx < 0 ? 0 : (u32)(lrx * 4.0f), yl = lry < 0 ? 0 : (u32)(lry * 4.0f);
        if (xl > 0xFFF) xl = 0xFFF;
        if (yl > 0xFFF) yl = 0xFFF;
        if (xh > 0xFFF) xh = 0xFFF;
        if (yh > 0xFFF) yh = 0xFFF;
        rect[0] = ((u64)((flip ? 0xE5000000u : 0xE4000000u) | (xl << 12) | yl) << 32) | ((g->texTile & 7) << 24) | (xh << 12) | yh;
        rect[1] = ((u64)(((u32)S << 16) | T) << 32) | ((u32)sdsdx << 16) | sdtdy;
        out_rdp(g, rect, 2);
    }
}

static void run_f5rogue(H64Gfx *g, u32 w0, u32 w1)
{
    u32 cmd = w0 >> 24;
    switch (cmd)
    {
    case 0x00: break;                                              // G_SPNOOP
    case 0x01:          // G_MTX (the projection's address is kept for TEXRECT_GEN)
    {
        u32 p = (w0 >> 16) & 0xFF;
        if (p & 1) g->f5PerspAddr = w1 & 0x00FFFFFF;
        do_matrix(g, w1, p & 1, p & 2, p & 4);
        break;
    }
    case 0x02: g->f5ColorBase = seg_to_phys(g, w1); break;        // vertex colour table
    case 0x03:          // G_MOVEMEM: the data follows the command
    {
        u32 cur = f5_cur(g), idx = (w0 >> 16) & 0xFF;
        if (idx == 0x80) { g->f5VpAddr = cur + 8; set_viewport(g, cur + 8); }
        else if (idx == 0x82) { g->f5TexScale[0] = rd32(g, cur + 16); g->f5TexScale[1] = rd32(g, cur + 12); }
        g->pc[g->pci] += 16;
        break;
    }
    case 0x04:          // G_VTX: 8-byte vertices into the first slots
    {
        u32 n = (w0 >> 10) & 0x3F, a = seg_to_phys(g, w1), i;
        for (i = 0; i < n; i++) f5_vertex(g, i, rd16(g, a + i * 8), rd16(g, a + i * 8 + 2), rd16(g, a + i * 8 + 4));
        break;
    }
    case 0x05: f5_trigen(g, w0); break;                            // TRI_GEN
    case 0x06:          // G_DL
        call_dl(g, w1, 1);
        if (!g->abort) f5_enter(g);
        break;
    case 0x07:          // G_BRANCH_DL
        call_dl(g, w1, 0);
        f5_enter(g);
        break;
    case 0xB3: case 0xBE:   // G_SETOTHERMODE_L/H_EX: the mask follows the command
    {
        u32 mask = rd32(g, g->pc[g->pci]);
        if (cmd == 0xBE) g->omH = (g->omH & mask) | w1;
        else g->omL = (g->omL & mask) | w1;
        g->pc[g->pci] += 8;
        out_othermode(g);
        break;
    }
    case 0xB4: f5_tri(g, w0, w1, 1); break;                        // G_TRI2
    case 0xB5:          // G_JUMPSWDL: to the list's link word
    {
        u32 a = g->f5dl[g->pci];
        g->pc[g->pci] = a;
        f5_enter(g);
        break;
    }
    case 0xB6: g->geomRaw &= ~w1; break;                           // G_CLEARGEOMETRYMODE
    case 0xB7: g->geomRaw |= w1; break;                            // G_SETGEOMETRYMODE
    case 0xB8: end_dl(g); break;                                   // G_ENDDL
    case 0xB9: set_othermode(g, 0, (w0 >> 8) & 0xFF, w0 & 0xFF, w1); break;
    case 0xBA: set_othermode(g, 1, (w0 >> 8) & 0xFF, w0 & 0xFF, w1); break;
    case 0xBB: set_texture(g, w1, (w0 >> 11) & 7, (w0 >> 8) & 7, w0 & 0xFF); break;
    case 0xBC:          // G_MOVEWORD
        switch (w0 & 0xFF)
        {
        case 0x04:
        {
            int r = (s16)w1;
            if (r < 0) r = -r;
            if (r >= 1 && r <= 6) g->clipRatio = (float)r;
            break;
        }
        case 0x06: g->segments[(((w0 >> 8) & 0xFFFF) >> 2) & 0xF] = w1 & 0x00FFFFFF; break;
        case 0x08: g->fogMul = (float)(s32)w1 / 256.0f; break;   // s15.16 (as the other microcodes' value / 256)
        case 0x0A: g->fogOff = (float)(s32)w1 / 256.0f; break;
        default: break;
        }
        break;
    case 0xBD: f5_texrect_gen(g, w0); break;                       // TEXRECT_GEN
    case 0xBF: f5_tri(g, w0, w1, 0); break;                        // G_TRI1
    case 0xE4: case 0xE5:   // TEXRECT: its second half follows as one 8-byte word pair
    {
        u32 a = g->pc[g->pci];
        u64 w[2];
        w[0] = ((u64)w0 << 32) | w1;
        w[1] = ((u64)rd32(g, a) << 32) | rd32(g, a + 4);
        g->pc[g->pci] += 8;
        out_rdp(g, w, 2);
        break;
    }
    default:
        if (cmd >= 0xC0) rdp_passthrough(g, w0, w1);
        else if (!(g->warned & 2)) { H64_WARN("[gfx] F3DSWRS command %02X not handled", cmd); g->warned |= 2; }
        break;
    }
}
