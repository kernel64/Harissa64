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

// ---------------------------------------------------------------------------
// Indiana Jones and the Infernal Machine, Star Wars Episode I: Battle for
// Naboo. Studied in GLideN64's uCodes/F5Indi_Naboo.cpp (GPL v2, microcode
// decoding by olivieryuyu) and rewritten. Much of this microcode works on its
// DMEM: lighting tables, vertex buffers (40 bytes a vertex from 0x600),
// generated terrain, command buffers. It is kept here in H64Gfx.f5dm in
// GLideN64's layout (host-order 32-bit words, read little-endian), so that
// the decoded offsets apply unchanged on every host; dmw/dmh/dmd read and
// write that layout, rnb/rnh/rnw read RDRAM the same way.
static H64_INLINE u8 *dmp(H64Gfx *g, u32 o) { return &g->f5dm[o & 0xFFF]; }
static H64_INLINE u8 dmb(H64Gfx *g, u32 o) { return g->f5dm[o & 0xFFF]; }
static u16 dmh(H64Gfx *g, u32 o) { return (u16)(dmb(g, o) | dmb(g, o + 1) << 8); }
static u32 dmw(H64Gfx *g, u32 o) { return (u32)dmh(g, o) | (u32)dmh(g, o + 2) << 16; }
static u64 dmd(H64Gfx *g, u32 o) { return (u64)dmw(g, o) | (u64)dmw(g, o + 4) << 32; }
static void dmh_set(H64Gfx *g, u32 o, u16 v) { g->f5dm[o & 0xFFF] = (u8)v; g->f5dm[(o + 1) & 0xFFF] = (u8)(v >> 8); }
static void dmw_set(H64Gfx *g, u32 o, u32 v) { dmh_set(g, o, (u16)v); dmh_set(g, o + 2, (u16)(v >> 16)); }
static void dmd_set(H64Gfx *g, u32 o, u64 v) { dmw_set(g, o, (u32)v); dmw_set(g, o + 4, (u32)(v >> 32)); }
static H64_INLINE u8 rnb(H64Gfx *g, u32 o) { return rd8(g, o ^ 3); }
static u16 rnh(H64Gfx *g, u32 o) { return (u16)(rnb(g, o) | rnb(g, o + 1) << 8); }
static u32 rnw(H64Gfx *g, u32 o) { return (u32)rnh(g, o) | (u32)rnh(g, o + 2) << 16; }
static void rn_store8(H64Gfx *g, u32 o, u8 v) { g->sys->rdram[(o ^ 3) & 0x7FFFFF] = v; }
static void dm_from_rdram(H64Gfx *g, u32 d, u32 s, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) g->f5dm[(d + i) & 0xFFF] = rnb(g, s + i);
}

// A vertex as the microcode keeps it: y, x, flag, z halfwords (GLideN64's SWVertex).
struct F5Sw { s16 y, x; u16 flag; s16 z; };
static void sw_from_dm(H64Gfx *g, u32 a, F5Sw *v)
{
    v->y = (s16)dmh(g, a); v->x = (s16)dmh(g, a + 2); v->flag = dmh(g, a + 4); v->z = (s16)dmh(g, a + 6);
}
static void sw_to_dm(H64Gfx *g, u32 a, const F5Sw *v)
{
    dmh_set(g, a, (u16)v->y); dmh_set(g, a + 2, (u16)v->x); dmh_set(g, a + 4, v->flag); dmh_set(g, a + 6, (u16)v->z);
}
static void sw_from_rdram(H64Gfx *g, u32 a, F5Sw *v)
{
    v->y = (s16)rnh(g, a); v->x = (s16)rnh(g, a + 2); v->flag = rnh(g, a + 4); v->z = (s16)rnh(g, a + 6);
}

static H64_INLINE u32 f5_min(u32 a, u32 b) { return a < b ? a : b; }

// A screen-space rectangle (float corners) as a TEXRECT; off the top or left
// it starts at 0 (S and T were computed for that).
static void f5_emit_rect(H64Gfx *g, float ulx, float uly, float lrx, float lry, u32 tile, u16 S, u16 T, u16 dsdx, u16 dtdy, int flip)
{
    u64 rect[2];
    u32 xh = ulx < 0 ? 0 : (u32)(ulx * 4.0f), yh = uly < 0 ? 0 : (u32)(uly * 4.0f);
    u32 xl = lrx < 0 ? 0 : (u32)(lrx * 4.0f), yl = lry < 0 ? 0 : (u32)(lry * 4.0f);
    if (xl > 0xFFF) xl = 0xFFF;
    if (yl > 0xFFF) yl = 0xFFF;
    if (xh > 0xFFF) xh = 0xFFF;
    if (yh > 0xFFF) yh = 0xFFF;
    rect[0] = ((u64)((flip ? 0xE5000000u : 0xE4000000u) | (xl << 12) | yl) << 32) | ((tile & 7) << 24) | (xh << 12) | yh;
    rect[1] = ((u64)(((u32)S << 16) | T) << 32) | ((u32)dsdx << 16) | dtdy;
    out_rdp(g, rect, 2);
}

static void f5_prim_depth(H64Gfx *g, const GfxVtx *v)
{
    float z = (g->vtrans[2] + g->vscale[2] * v->z / v->w) * 32.0f;
    out_rdp1(g, 0xEE000000u, (z < 0 ? 0 : z > 32767.0f ? 32767 : (u32)z) << 16);
}

static void run_f5indi(H64Gfx *g, u32 w0, u32 w1);

// The sub display list whose address the microcode keeps at 0x58C (texture
// and mode changes for the next primitive), run at once (GLideN64 F5INDI_DoSubDList).
static void f5i_dmem_cmds(H64Gfx *g, u32 a);

// A Naboo polygon ran in a mode of its own (blended with fog, two cycles):
// the saved mode and combiner come back before anything else is drawn, as the
// LLE RSP's output shows (left in that mode, the terrain was drawn with the
// detail texture: brown squares over the snow).
static void f5n_restore(H64Gfx *g)
{
    if (!g->f5Restore) return;
    g->f5Restore = 0;
    f5i_dmem_cmds(g, 0xE50);
}

static void f5i_sub_dl(H64Gfx *g)
{
    u32 dl = dmw(g, 0x58C) & 0x00FFFFFF, n = 0;
    int base = g->pci;
    f5n_restore(g);
    if (!dl || g->pci >= GFX_PC_MAX - 1) return;
    g->pci++;
    g->f5dl[g->pci] = rd32(g, dl) & 0x00FFFFFF;
    g->pc[g->pci] = dl + 8;
    while (!g->abort && n++ < 100000)
    {
        u32 a = g->pc[g->pci], w0 = rd32(g, a), w1 = rd32(g, a + 4), next;
        if (w0 == 0xB8000000u && w1 == 0xFFFFFFFFu) break;
        next = rd32(g, a + 8) >> 24;
        g->pc[g->pci] = a + 8;
        run_f5indi(g, w0, w1);
        if (g->pci <= base) { g->halt = 0; break; }   // ended by a G_ENDDL of its own
        if (next == 0xBD)
        {
            // Naboo: the command before G_BD is kept for later.
            dmw_set(g, 0xE58, w0);
            dmw_set(g, 0xE5C, w1);
            break;
        }
    }
    g->pci = base;
    dmw_set(g, 0x58C, 0);
}

// Two commands kept in DMEM, run as if they came from a display list.
static void f5i_dmem_cmds(H64Gfx *g, u32 a)
{
    run_f5indi(g, dmw(g, a), dmw(g, a + 4));
    run_f5indi(g, dmw(g, a + 8), dmw(g, a + 12));
}

// Loads vertex slots from microcode-layout vertices.
static void f5i_vertices(H64Gfx *g, const F5Sw *v, u32 first, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) f5_vertex(g, first + i, v[i].x, v[i].y, v[i].z);
}

static int f5i_clipped(H64Gfx *g, u32 a, u32 b, u32 c)
{
    if (a >= GFX_VTX_MAX || b >= GFX_VTX_MAX || c >= GFX_VTX_MAX) return 1;
    return (g->vtx[a].clip & g->vtx[b].clip & g->vtx[c].clip & (CL_POSX | CL_NEGX | CL_POSY | CL_NEGY | CL_W)) != 0;
}

// ---- lighting into the colour buffer at 0xD40
static void f5i_light_overlay1(H64Gfx *g, u32 w0, u32 w1)
{
    u8 *D = g->f5dm;
    u32 S = ((((w1 >> 24) & 0x7F) << 2) + 4) & 0xFFF8, lightAddr = 0xB10, dataAddr = (w0 >> 8) & 0xFFF, i, j;
    u8 L = dmb(g, 0x58B ^ 3);
    if (S > 0x2C0) S = 0x2C0;
    memset(D + 0xD40, 0, S);
    if (L == 0) { dmd_set(g, 0x5B0, 0); return; }
    for (;;)
    {
        u32 dst = 0xD40, data = dataAddr;
        if (dmb(g, lightAddr) == 0)
        {
            for (i = 0; i < (S >> 3); i++)
            {
                u32 f = 0x158;
                u32 k;
                for (k = 0; k < 2; k++)
                {
                    *dmp(g, dst) = (u8)((dmb(g, data) * dmb(g, f)) >> 8);
                    for (j = 1; j < 4; j++)
                        *dmp(g, dst + j) = (u8)f5_min(((dmb(g, data + j) * dmb(g, f + j) * dmb(g, lightAddr + j)) >> 16) + dmb(g, dst + j), 0xFF);
                    f += 4; dst += 4; data += 4;
                }
            }
        }
        else
        {
            u32 offIdx = 0;
            u64 pos = dmd(g, 0x5B0);
            u32 factors[2];
            factors[0] = dmw(g, 0x158); factors[1] = dmw(g, 0x15C);
            for (i = 0; i < (S >> 3); i++)
            {
                u32 va[2];
                switch (pos & 3)
                {
                case 0: va[0] = va[1] = lightAddr - 0x10; break;
                case 1: va[0] = lightAddr - 0x08; va[1] = lightAddr - 0x10; break;
                case 2: va[0] = lightAddr - 0x10; va[1] = lightAddr - 0x08; break;
                default: va[0] = va[1] = lightAddr - 0x08; break;
                }
                pos >>= 2;
                for (j = 0; j < 2; j++)
                {
                    s16 X1 = (s16)(dmw(g, va[j]) >> 16), Y1 = (s16)dmw(g, va[j]), Z1 = (s16)(dmw(g, va[j] + 4) >> 16);
                    u32 a = 0x170 + dmb(g, 0x380 + (offIdx ^ 3));
                    s16 X, Y, Z;
                    u64 T, K;
                    u32 P, P1 = 0, D2, E, F, J, V;
                    offIdx++;
                    a &= 0x0FFC;
                    X = (s16)(dmw(g, a) >> 16); Y = (s16)dmw(g, a); Z = (s16)(dmw(g, a + 4) >> 16);
                    X = (s16)(X - X1); Y = (s16)(Y - Y1); Z = (s16)(Z - Z1);
                    T = dmw(g, lightAddr + 4);
                    T = 0xFFFF000000000000ull | (T << 16);
                    K = (u64)(s64)(X * X + Y * Y + Z * Z) * T;
                    P = (u32)(K >> 32);
                    if ((P & 0xFFFF0000u) == 0xFFFF0000u) P1 = P & 0xFFFF;
                    V = dmw(g, lightAddr);
                    D2 = ((V >> 24) * P1) >> 16;
                    E = (((V >> 16) & 0xFF) * P1) >> 16;
                    F = (((V >> 8) & 0xFF) * P1) >> 16;
                    J = ((factors[j] & 0xFF) * dmb(g, data + (0x3 ^ 3))) >> 8;
                    if (dmb(g, lightAddr) == 4)
                    {
                        *dmp(g, dst) = (u8)J;
                        *dmp(g, dst + 1) = (u8)f5_min(dmb(g, dst + 1) + F, 0xFF);
                        *dmp(g, dst + 2) = (u8)f5_min(dmb(g, dst + 2) + E, 0xFF);
                        *dmp(g, dst + 3) = (u8)f5_min(dmb(g, dst + 3) + D2, 0xFF);
                    }
                    else
                    {
                        D2 += ((factors[j] >> 24) * dmb(g, data + (0x0 ^ 3))) >> 8;
                        E += (((factors[j] >> 16) & 0xFF) * dmb(g, data + (0x1 ^ 3))) >> 8;
                        F += (((factors[j] >> 8) & 0xFF) * dmb(g, data + (0x2 ^ 3))) >> 8;
                        *dmp(g, dst) = (u8)J;
                        *dmp(g, dst + 1) = (u8)f5_min(F >> 1, 0xFF);
                        *dmp(g, dst + 2) = (u8)f5_min(E >> 1, 0xFF);
                        *dmp(g, dst + 3) = (u8)f5_min(D2 >> 1, 0xFF);
                    }
                    dst += 4;
                    data += 4;
                }
            }
        }
        if (--L == 0)
        {
            dmd_set(g, 0x5B0, 0);
            if (dmw(g, 0x5A8) == 0xFFFFFFFFu) return;
            dst = 0xD40;
            for (i = 0; i < (S >> 2); i++, dst += 4)
                for (j = 0; j < 4; j++) *dmp(g, dst + j) = (u8)((dmb(g, dst + j) * dmb(g, 0x5A8 + j)) >> 8);
            return;
        }
        lightAddr += 0x18;
    }
}

static void f5i_light_overlay2(H64Gfx *g, u32 w0, u32 w1)
{
    u8 *D = g->f5dm;
    u32 S = ((((w1 >> 24) & 0x7F) << 2) + 4) & 0xFFF8, lightAddr = 0xB10, dataAddr = (w0 >> 8) & 0xFFF, i, j, k;
    u8 L = dmb(g, 0x58B ^ 3);
    if (S > 0x2B8) S = 0x2B8;
    memset(D + 0xD40, 0, S + 8);
    if (L == 0) { dmd_set(g, 0x5B0, 0); return; }
    for (;;)
    {
        u32 dst = 0xD40, data = dataAddr;
        u8 type = dmb(g, lightAddr);
        if (type == 0)
        {
            for (i = 0; i < (S >> 3); i++)
            {
                u32 f = 0x158, h;
                for (h = 0; h < 2; h++)
                {
                    *dmp(g, dst) = (u8)((dmb(g, data) * dmb(g, f)) >> 8);
                    for (j = 1; j < 4; j++)
                        *dmp(g, dst + j) = (u8)f5_min(((dmb(g, data + j) * dmb(g, f + j) * dmb(g, lightAddr + j)) >> 16) + dmb(g, dst + j), 0xFF);
                    f += 4; dst += 4; data += 4;
                }
            }
        }
        else if (type == 1)
        {
            u32 f2 = 0x380;
            u64 pos = dmd(g, 0x5B0);
            u32 factors[2];
            factors[0] = dmw(g, 0x158); factors[1] = dmw(g, 0x15C);
            for (i = 0; i < (S >> 3); i++)
            {
                u32 va[2];
                switch (pos & 3)
                {
                case 0: va[0] = va[1] = lightAddr - 0x10; break;
                case 1: va[0] = lightAddr - 0x08; va[1] = lightAddr - 0x10; break;
                case 2: va[0] = lightAddr - 0x10; va[1] = lightAddr - 0x08; break;
                default: va[0] = va[1] = lightAddr - 0x08; break;
                }
                pos >>= 2;
                for (j = 0; j < 2; j++)
                {
                    u32 V = dmw(g, va[j]);
                    s16 X = (s16)(V >> 16), Y = (s16)V, Z;
                    u32 I, D2, E, F, K2, J, H, G2;
                    Z = (s16)(dmw(g, va[j] + 4) >> 16);
                    V = dmw(g, f2);
                    I = (u32)(((X * (s8)(V >> 24) + Y * (s8)(V >> 16) + Z * (s8)(V >> 8)) >> 8) & 0xFFFF);
                    if (I & 0x8000) I = 0;
                    V = dmw(g, lightAddr);
                    D2 = ((V >> 24) * I) >> 8;
                    E = (((V >> 16) & 0xFF) * I) >> 8;
                    F = (((V >> 8) & 0xFF) * I) >> 8;
                    V = factors[j];
                    K2 = (V & 0xFF) * dmb(g, data); data++;
                    J = ((V >> 8) & 0xFF) * dmb(g, data); data++;
                    H = ((V >> 16) & 0xFF) * dmb(g, data); data++;
                    G2 = (V >> 24) * dmb(g, data); data++;
                    *dmp(g, dst) = (u8)(K2 >> 8);
                    *dmp(g, dst + 1) = (u8)f5_min(dmb(g, dst + 1) + ((F * J) >> 22), 0xFF);
                    *dmp(g, dst + 2) = (u8)f5_min(dmb(g, dst + 2) + ((E * H) >> 22), 0xFF);
                    *dmp(g, dst + 3) = (u8)f5_min(dmb(g, dst + 3) + ((D2 * G2) >> 22), 0xFF);
                    dst += 4;
                    f2 += 4;
                }
            }
        }
        else
        {
            u32 off = 0x380, va = lightAddr - 0x10;
            for (i = 0; i < (S >> 3); i++)
            {
                for (j = 0; j < 2; j++)
                {
                    s16 X1 = (s16)(dmw(g, va) >> 16), Y1 = (s16)dmw(g, va), Z1 = (s16)(dmw(g, va + 4) >> 16);
                    u32 o = dmw(g, off + j * 4), a = 0x170 + (o & 0xFF), P, P1 = 0;
                    s16 X = (s16)(dmw(g, a) >> 16), Y = (s16)dmw(g, a), Z = (s16)(dmw(g, a + 4) >> 16);
                    u64 T, K;
                    X = (s16)(X - X1); Y = (s16)(Y - Y1); Z = (s16)(Z - Z1);
                    T = dmw(g, lightAddr + 4);
                    T = 0xFFFF000000000000ull | (T << 16);
                    K = (u64)(s64)(X * X + Y * Y + Z * Z) * T;
                    P = (u32)(K >> 32);
                    if ((P & 0xFFFF0000u) == 0xFFFF0000u) P1 = P & 0xFFFF;
                    *dmp(g, dst) = (u8)((dmb(g, data) * dmb(g, 0x158 + j * 4)) >> 8);
                    for (k = 1; k < 4; k++)
                        *dmp(g, dst + k) = (u8)f5_min(((((dmb(g, lightAddr + k) * P1) >> 16) * dmb(g, data + k) * dmb(g, 0x158 + j * 4 + k) + 0x8000) >> 16) + dmb(g, dst + k), 0xFF);
                    dst += 4;
                    data += 4;
                }
                off += 8;
            }
        }
        if (--L == 0) { dmd_set(g, 0x5B0, 0); return; }
        lightAddr += 0x18;
    }
}

static void f5i_light_basic(H64Gfx *g, u32 w0, u32 w1)
{
    s32 count = (w1 >> 24) & 0x7F;
    u32 data = (w0 >> 8) & 0xFFF, dst = 0xD40, j;
    while (count > 0)
    {
        for (j = 0; j < 8; j++) *dmp(g, dst + j) = (u8)((dmb(g, data + j) * dmb(g, 0x158 + j)) >> 8);
        dst += 8; data += 8;
        for (j = 0; j < 8; j++) *dmp(g, dst + j) = (u8)((dmb(g, data + j) * dmb(g, 0x158 + j)) >> 8);
        dst += 8; data += 8;
        count -= 4;
    }
}

static void f5i_rebuild_colors(H64Gfx *g, u32 w0, u32 w1)
{
    u32 addr = (w0 >> 8) & 0xFFF, count, i, res[256];
    count = addr < 0x588 ? f5_min(w1 >> 24, (0x588 - addr) >> 2) : 0;
    for (i = 0; i < count; i++)
    {
        u16 V = dmh(g, addr + 2 * (i ^ 1));
        res[i] = ((u32)((V >> 8) & 0xF8) << 24) | ((u32)((V & 0x7E0) >> 3) << 16) | ((u32)((V & 0x1F) << 3) << 8) | 0xFF;
    }
    for (i = 0; i < count; i++) dmw_set(g, addr + i * 4, res[i]);
}

static void f5i_movemem(H64Gfx *g, u32 w0, u32 w1)
{
    if (w1 & 0x00FFFFFF)
    {
        // Direct DMA from RDRAM.
        u32 src = w1 & 0x00FFFFFF, dst = (w0 >> 8) & 0xFFF, n = ((w0 & 0xFF) + 1) >> 2, i;
        for (i = 0; i < n; i++) dmw_set(g, dst + i * 4, rnw(g, src + i * 4));
    }
    else if (w0 & 0xFF)
    {
        // From the display list's data stream (G_SET_DLIST_ADDR's state at 0xFD4..0xFDC).
        s32 A0 = (s32)(w0 & 0xFF), A3 = A0 + 1, A1;
        u32 A2 = dmw(g, 0xFDC), V1 = (w0 >> 8) & 0xFFF, V0, guard = 0;
        A1 = (s32)A2 - A3;
        while (A1 < 0 && guard++ < 64)
        {
            u32 nw1;
            V0 = dmw(g, 0xFD4);
            A3 = A0 - (s32)A2;
            dm_from_rdram(g, V1, V0 & 0xFFFFFF, A2 & 0xFFF);
            V1 += A2;
            A0 = A3;
            nw1 = dmw(g, 0xFD8);
            dmw_set(g, 0xFD8, rnw(g, nw1 & 0xFFFFFF));
            dmw_set(g, 0xFDC, 0x100);
            dmw_set(g, 0xFD4, nw1 + 8);
            A2 = dmw(g, 0xFDC);
            A3 = A0 + 1;
            A1 = (s32)A2 - A3;
        }
        V0 = dmw(g, 0xFD4);
        dm_from_rdram(g, V1, V0 & 0xFFFFFF, (u32)(A0 + 1) & 0xFFF);
        dmw_set(g, 0xFDC, (u32)A1);
        dmw_set(g, 0xFD4, V0 + (u32)A3);
    }
    switch (w0 & 0x00F00000)
    {
    case 0x00000000:
        if (w0 == 0x0101300Fu) set_viewport(g, w1);
        else if (w0 == 0x0105C03Fu) force_matrix(g, w1);
        else if (w0 == 0x010E403Fu)
        {
            u32 i;
            load_matrix(g, w1 & 0x00FFFFFF, g->f5MtxGen);
            for (i = 0; i < 16; i++)
                g->f5StAdj[i] = (s32)(((u32)dmh(g, 0xE40 + 2 * (i ^ 1)) << 16) | dmh(g, 0xE60 + 2 * (i ^ 1)));
        }
        break;
    case 0x00100000:   // vertices in DMEM (moved by the vertex at 0x128 when it is not 0)
    {
        u32 n = (w1 >> 24) & 0x7F, a = (w0 >> 8) & 0xFFF, i;
        int adj = dmd(g, 0x128) != 0;
        u16 X0 = dmh(g, 0x128 + 2), Y0 = dmh(g, 0x128), Z0 = dmh(g, 0x128 + 6);
        for (i = 0; i < n && i < GFX_VTX_MAX; i++)
        {
            F5Sw v;
            if (adj)
            {
                v.x = (s16)(dmh(g, a + 2) + X0);
                v.y = (s16)(dmh(g, a) + Y0);
                v.z = (s16)(dmh(g, a + 6) + Z0);
            }
            else sw_from_dm(g, a, &v);
            f5_vertex(g, i, v.x, v.y, v.z);
            a += 8;
        }
        break;
    }
    case 0x00300000: *dmp(g, 0x58B ^ 3) = (u8)(w1 >> 24); break;   // number of lights
    case 0x00500000:   // vertices from RDRAM by index (6 bytes each), plus the vertex at 0x128
    {
        u32 n = (w1 >> 24) & 0x7F, a = (w0 >> 8) & 0xFFF, base = dmw(g, 0x154), i;
        u16 X0 = dmh(g, 0x128 + 2), Y0 = dmh(g, 0x128), Z0 = dmh(g, 0x128 + 6);
        for (i = 0; i < n && i < GFX_VTX_MAX; i++)
        {
            u32 ra = dmh(g, a + 2 * (i ^ 1)) * 6 + base, b = ra & 0x00FFFFF8, sh = (ra & 7) >> 1;
            s16 X = (s16)(rnh(g, b + 2 * ((sh + 0) ^ 1)) + X0);
            s16 Y = (s16)(rnh(g, b + 2 * ((sh + 1) ^ 1)) + Y0);
            s16 Z = (s16)(rnh(g, b + 2 * ((sh + 2) ^ 1)) + Z0);
            f5_vertex(g, i, X, Y, Z);
        }
        break;
    }
    default:
        if (((w0 >> 8) & 0xFFF) == 0x480)
        {
            u32 F = w0 >> 22, C;
            if (F & 2) f5i_rebuild_colors(g, w0, w1);
            C = (w1 >> 24) | dmb(g, 0x58B ^ 3);
            if ((C & 0x80) == 0)
            {
                if (F & 1) f5i_light_overlay1(g, w0, w1);
                else f5i_light_overlay2(g, w0, w1);
            }
            else f5i_light_basic(g, w0, w1);
        }
        break;
    }
}

// Texture coordinates from the vertices' positions (environment mapping).
static void f5i_calc_st(H64Gfx *g, u32 cur, u32 num, u32 *st)
{
    const s32 *m = g->f5StAdj;
    u32 i;
    for (i = 0; i < num; i++)
    {
        u32 idx = rd32(g, cur + (i + 4) * 4) & 0xFF, c = 0x170 + idx;
        s16 X = (s16)dmh(g, c + 2), Y = (s16)dmh(g, c), Z = (s16)dmh(g, c + 6);
        s32 X1 = X * m[0] + Y * m[4] + Z * m[8] + m[12];
        s32 Y1 = X * m[1] + Y * m[5] + Z * m[9] + m[13];
        s32 Z1 = X * m[2] + Y * m[6] + Z * m[10] + m[14];
        u64 X2, Y2, Z2, R;
        u32 R1, Dd, den, V, W, S, T;
        X1 -= (s32)((u32)dmh(g, 0xF0 + 2) << 16);
        Y1 -= (s32)((u32)dmh(g, 0xF0) << 16);
        Z1 -= (s32)((u32)dmh(g, 0xF0 + 6) << 16);
        X2 = (u64)(s64)X1; Y2 = (u64)(s64)Y1; Z2 = (u64)(s64)Z1;
        R = ((X2 * X2) >> 16) + ((Y2 * Y2) >> 16) + ((Z2 * Z2) >> 16);
        R1 = (u32)R;
        if (R > 0x0000FFFFFFFFull) R1 = 0x7FFF0000u | (u32)(R & 0xFFFF);
        Dd = (u32)sqrt((double)R1);
        den = Dd * 0x300;
        Dd = den ? 0xFFFFFFFFu / den : 0xFFFFFFFFu;
        V = (u32)(((u64)Dd * X2) >> 16);
        W = (u32)(((u64)Dd * Y2) >> 16);
        S = (u32)(((u64)V * dmh(g, 0xEC + 2)) >> 16);
        T = (u32)(((u64)W * dmh(g, 0xEC)) >> 16);
        st[i] = (S << 16) | (T & 0xFFFF);
    }
}

static void f5i_color_st(H64Gfx *g, u32 vi, u32 rgba, int useTex, u32 st, int persp)
{
    GfxVtx *v;
    if (vi >= GFX_VTX_MAX) return;
    v = &g->vtx[vi];
    v->r = s_u8f[rgba >> 24];
    v->g = s_u8f[(rgba >> 16) & 0xFF];
    v->b = s_u8f[(rgba >> 8) & 0xFF];
    if (!(geom(g) & GM_FOG)) v->a = s_u8f[rgba & 0xFF];
    if (useTex)
    {
        s16 s = (s16)(st >> 16), t = (s16)st;
        v->s = persp ? (float)s : (float)s * 2.0f;
        v->t = persp ? (float)t : (float)t * 2.0f;
    }
}

// Draws a triangle with this microcode's culling: textured ones cull the
// back faces when both are asked, untextured ones never cull.
static void f5i_draw(H64Gfx *g, u32 a, u32 b, u32 c, int useTex)
{
    u32 gm = g->geomRaw;
    if (useTex) { if ((g->geomRaw & 0x3000) == 0x3000) g->geomRaw &= ~0x1000u; }
    else g->geomRaw &= ~0x3000u;
    triangle(g, a, b, c);
    g->geomRaw = gm;
}

static void f5i_tri(H64Gfx *g, u32 w0, u32 w1, int two)
{
    u32 cur = f5_cur(g), p2 = rd32(g, cur + 8), p3 = rd32(g, cur + 12), v[4], col[4], st[4], i, n = two ? 4 : 3;
    int useTex = (w0 & 0x0200) != 0, persp = (g->omH & 0x80000) != 0, d1, d2 = 0;
    f5n_restore(g);
    v[0] = (((w1 >> 16) & 0xFFF) - 0x600) / 40;
    v[1] = ((w1 & 0xFFF) - 0x600) / 40;
    v[2] = (((p3 >> 16) & 0xFFF) - 0x600) / 40;
    v[3] = ((p3 & 0xFFF) - 0x600) / 40;
    col[0] = dmw(g, 0xD40 + ((p2 >> 16) & 0xFF));
    col[1] = dmw(g, 0xD40 + ((p2 >> 8) & 0xFF));
    col[2] = dmw(g, 0xD40 + (p2 & 0xFF));
    col[3] = dmw(g, 0xD40 + (p2 >> 24));
    if (w0 & 0x0800) f5i_calc_st(g, cur, n, st);
    else for (i = 0; i < n; i++) st[i] = rd32(g, cur + 16 + i * 4);
    for (i = 0; i < n; i++) f5i_color_st(g, v[i], col[i], useTex, st[i], persp);
    if (useTex) g->pc[g->pci] += 16;
    g->pc[g->pci] += 8;
    d1 = !f5i_clipped(g, v[0], v[1], v[2]);
    if (two) d2 = !f5i_clipped(g, v[0], v[2], v[3]);
    if (d1 || d2) f5i_sub_dl(g);
    if (d1) f5i_draw(g, v[0], v[1], v[2], useTex);
    if (d2) f5i_draw(g, v[0], v[2], v[3], useTex);
}

// ---- Indiana Jones' generated vertices, particles and sprites
static void f5i_gen_vertices(H64Gfx *g, u32 w0, u32 w1)
{
    u32 A = (w0 & 0x0000FFFF) | (w1 & 0xFFFF0000u), B = 1, count = 0;
    g->f5Mtx = &g->f5MtxGen[0][0];
    while (A != 0 && count < 32)
    {
        u32 C = A & B;
        if (C)
        {
            F5Sw v;
            sw_from_dm(g, 0x170 + count * 8, &v);
            f5_vertex(g, count, v.x, v.y, v.z);
            A ^= C;
        }
        B <<= 1;
        count++;
    }
    g->f5Mtx = 0;
}

static void f5i_gen_particles(H64Gfx *g)
{
    u32 cur = f5_cur(g), p[6], L, U, corr, M, vtxAddr = 0x170, lightAddr = 0xB00, factorAddr = 0x380, colorAddr = 0x480;
    u32 count = 0, Q = 0, i, clock = g->sys->vi.frames / 60;
    for (i = 0; i < 6; i++) p[i] = rd32(g, cur + i * 4);
    {
        u32 a = p[3] & 0xFFFFFF;
        for (i = 0; i < 8; i++) *dmp(g, 0xC10 + i) = rnb(g, a + i);
    }
    L = dmw(g, 0xC10);
    if (L == 0) return;
    U = L;
    dm_from_rdram(g, 0x170, p[2] & 0xFFFFFF, 256);
    corr = p[0] & 0xFFFF;
    M = p[5] & 0xFFFF;
    while (U != 0 && count < 32)
    {
        F5Sw vx;
        u16 F;
        int endCycle = 0;
        sw_from_dm(g, vtxAddr, &vx);
        F = (u16)((((u32)dmh(g, lightAddr + 2) << 12) + dmh(g, lightAddr) * corr) << 4 >> 16);
        if ((M & 0x2000) == 0)
        {
            s16 F1;
            u32 guard = 0;
            do
            {
                F = (u16)((((u32)dmh(g, lightAddr + 2) << 12) + dmh(g, lightAddr) * corr) << 4 >> 16);
                F1 = (s16)(F + 0xF000);
                if (F1 < 0)
                {
                    dmh_set(g, lightAddr + 2, F);
                    if (M & 0x40) vx.flag = (u16)((((F >> 4) & 0xFC) << 8) | (vx.flag & 0xFF));
                    if (M & 0x20) vx.flag = (u16)(((F >> 4) & 0xFC) | (vx.flag & 0xFF00));
                }
                else
                {
                    dmh_set(g, lightAddr + 2, 0);
                    if (M & 0x4000)
                    {
                        u32 V0 = dmw(g, 0xC18), V1 = dmw(g, 0xC1C), V2 = dmw(g, 0xC20), V3 = dmw(g, 0xC24);
                        vx.x = (s16)((clock & (V0 >> 16)) + (V2 >> 16));
                        vx.y = (s16)(((clock >> 3) & (V0 & 0xFFFF)) + (V2 & 0xFFFF));
                        vx.z = (s16)(((clock >> 6) & (V1 >> 16)) + (V3 >> 16));
                        vx.flag &= (u16)V1;
                    }
                    else
                    {
                        Q |= 1u << count;
                        endCycle = 1;
                        break;
                    }
                }
            } while (F1 >= 0 && guard++ < 16);
        }
        if (!endCycle)
        {
            u32 X = ((u32)(u16)vx.x << 12) + (u32)((s16)(p[4] >> 16) * (s32)corr);
            u32 Y = ((u32)(u16)vx.y << 12) + (u32)((s16)p[4] * (s32)corr);
            u32 Z = ((u32)(u16)vx.z << 12) + (u32)((s16)(p[5] >> 16) * (s32)corr);
            if (M & 0x0380)
            {
                u32 off = factorAddr - 0x380, f;
                if ((M & 0x0200) == 0) off = (M & 0x080) ? ((F >> 4) & 0xF8) : ((dmw(g, lightAddr + 4) >> 24) << 3);
                f = 0x380 + off;
                X += (u32)((s16)(dmw(g, f) >> 16) * (s32)corr);
                Y += (u32)((s16)dmw(g, f) * (s32)corr);
                Z += (u32)((s16)(dmw(g, f + 4) >> 16) * (s32)corr);
            }
            if (M & 0x1C00)
            {
                u32 off = colorAddr - 0x480, f;
                if ((M & 0x1000) == 0) off = (M & 0x0400) ? ((F >> 4) & 0xF8) : (((dmw(g, lightAddr + 4) >> 16) & 0xFF) << 3);
                f = 0x480 + off;
                X += (u32)((s16)(dmw(g, f) >> 16) * (s32)corr);
                Y += (u32)((s16)dmw(g, f) * (s32)corr);
                Z += (u32)((s16)(dmw(g, f + 4) >> 16) * (s32)corr);
            }
            vx.x = (s16)((X << 4) >> 16);
            vx.y = (s16)((Y << 4) >> 16);
            vx.z = (s16)((Z << 4) >> 16);
            if (M & 0x0008)
            {
                u32 V0 = dmw(g, 0xC00), V1 = dmw(g, 0xC04), V2 = dmw(g, 0xC08), V3 = dmw(g, 0xC0C);
                if (vx.x < (s16)(V0 >> 16)) vx.x = (s16)(vx.x + (s16)(V2 >> 16));
                if (vx.x > (s16)(V0 >> 16) + (s16)(V2 >> 16)) vx.x = (s16)(vx.x - (s16)(V2 >> 16));
                if (vx.y < (s16)V0) vx.y = (s16)(vx.y + (s16)V2);
                if (vx.y > (s16)V0 + (s16)V2) vx.y = (s16)(vx.y - (s16)V2);
                if (vx.z < (s16)(V1 >> 16)) vx.z = (s16)(vx.z + (s16)(V3 >> 16));
                if (vx.z > (s16)(V1 >> 16) + (s16)(V3 >> 16)) vx.z = (s16)(vx.z - (s16)(V3 >> 16));
            }
        }
        sw_to_dm(g, vtxAddr, &vx);
        vtxAddr += 8; lightAddr += 8; factorAddr += 8; colorAddr += 8;
        U >>= 1;
        count++;
    }
    for (i = 0; i < 256; i++) rn_store8(g, (p[2] & 0xFFFFFF) + i, dmb(g, 0x170 + i));
    if ((M & 0x04) == 0)
    {
        u32 a = p[3] & 0xFFFFFF, val = L & ~Q;
        // A native u32 store: N64 bytes in the word's order.
        h64_store_be32(g->sys->rdram + (a & 0x7FFFFC), val);
        for (i = 0; i < count * 8; i++) rn_store8(g, ((p[1] >> 8) & 0xFFFFFF) + i, dmb(g, 0xB00 + i));
    }
}

static u32 f5i_vertex_mask(H64Gfx *g, u32 mask, u32 lim, u32 vtxAddr)
{
    s16 XL = (s16)(dmw(g, lim) >> 16), YL = (s16)dmw(g, lim), ZL = (s16)(dmw(g, lim + 4) >> 16);
    s16 XR = (s16)(dmw(g, lim + 8) >> 16), YR = (s16)dmw(g, lim + 8), ZR = (s16)(dmw(g, lim + 12) >> 16);
    u32 S = 0xFFFFFFFEu, i;
    for (i = 0; i < 0x20; i++)
    {
        F5Sw v;
        sw_from_rdram(g, vtxAddr + i * 8, &v);
        if (v.x > XL && v.x < XR && v.y > YL && v.y < YR && v.z > ZL && v.z < ZR) mask &= S;
        S = (S << 1) | 1;
    }
    return mask;
}

static void f5i_texrect_gen(H64Gfx *g)
{
    u32 cur = f5_cur(g), p[10], i, vp = dmw(g, 0x130), vpx = (vp >> 17) & 0x7FFF, vpy = (vp >> 1) & 0x7FFF;
    u32 maxX, maxY, mp = dmw(g, 0x5C8), colorScale, vtxAddr;
    u64 MX, MY, xScale, yScale, Q, R;
    s32 count;
    for (i = 0; i < 10; i++) p[i] = rd32(g, cur + i * 4);
    maxX = (u32)(((u64)(p[0] & 0xFFFF) * vpx) >> 16);
    maxY = (u32)(((u64)(p[1] >> 16) * vpy) >> 16);
    MX = (p[4] & 0xFFFF0000u) | (p[5] >> 16);
    xScale = MX * (p[7] >> 16) * vpx;
    MY = ((u64)(p[4] << 16) & 0xFFFF0000u) | (p[5] & 0xFFFF);
    yScale = MY * (p[7] & 0xFFFF) * vpy;
    Q = (p[6] & 0xFFFF0000u) | (mp >> 16);
    R = ((u64)(p[6] << 16) & 0xFFFF0000u) | (mp & 0xFFFF);
    colorScale = p[3];
    count = (s32)((p[0] >> 16) & 0xFF);
    vtxAddr = p[2] & 0xFFFFFF;
    for (; count > 0; --count)
    {
        u32 K = p[8], B = p[9] & 0xFF, lim = 0xC28;
        for (i = 0; i < 0x20; i++)
        {
            F5Sw v;
            sw_from_rdram(g, vtxAddr + i * 8, &v);
            f5_vertex(g, i, v.x, v.y, v.z);
        }
        while (B != 0) { K = f5i_vertex_mask(g, K, lim, vtxAddr); B--; lim += 0x10; }
        for (i = 0; i < 0x20; i++)
        {
            const GfxVtx *v = &g->vtx[i];
            F5Sw sv;
            float sx, sy, offx, offy, ulx, uly, lrx, lry;
            u32 wi, invw, val, A, C, pc;
            u64 ox, oy, Q1, R1, U, V;
            s32 G, H;
            u16 dsdx, dtdy, U1, V1;
            s16 S, T;
            u32 D = K & 1;
            K >>= 1;
            if (!D) continue;
            if (v->clip & (CL_POSX | CL_NEGX | CL_POSY | CL_NEGY | CL_W)) continue;
            sw_from_rdram(g, vtxAddr + i * 8, &sv);
            sx = g->vtrans[0] + g->vscale[0] * v->x / v->w;
            sy = g->vtrans[1] - g->vscale[1] * v->y / v->w;
            wi = v->w >= 1.0f ? (u32)v->w : 1;
            invw = 0xFFFFFFFFu / wi;
            val = dmw(g, 0x480 + ((sv.flag >> 8) & 0xFF));
            A = val >> 16;
            C = val & 0xFFFF;
            ox = (xScale * invw) >> 32;
            ox = (ox * A) >> 32;
            if (ox > maxX) ox = maxX;
            if (ox < 4) ox = 4;
            offx = (u32)ox / 4.0f;
            ulx = sx - offx;
            G = ulx > 0.0f ? 0 : (s32)ulx;
            lrx = sx + offx > 0.0f ? sx + offx : 0.0f;
            if (lrx - ulx <= 0.0f) continue;
            oy = (yScale * invw) >> 32;
            oy = (oy * C) >> 32;
            if (oy > maxY) oy = maxY;
            if (oy < 4) oy = 4;
            offy = (u32)oy / 4.0f;
            uly = sy - offy;
            H = uly > 0.0f ? 0 : (s32)uly;
            lry = sy + offy > 0.0f ? sy + offy : 0.0f;
            if (lry - uly <= 0.0f) continue;
            Q1 = Q * 0x40 / ox;
            dsdx = (u16)(Q1 >> 16);
            R1 = R * 0x40 / oy;
            dtdy = (u16)(R1 >> 16);
            U = Q1 * 0x40 < 0x7FFFFFFFull ? Q1 * 0x40 : 0x7FFFFFFFull;
            U1 = (u16)(((0 - U) * (u64)(s64)G) >> 16);
            S = (s16)((U1 - 2) * 8);
            V = R1 * 0x40 < 0x7FFFFFFFull ? R1 * 0x40 : 0x7FFFFFFFull;
            V1 = (u16)(((0 - V) * (u64)(s64)H) >> 16);
            T = (s16)((V1 - 2) * 8);
            f5_prim_depth(g, v);
            pc = dmw(g, 0x380 + (sv.flag & 0xFF));
            out_rdp1(g, 0xFA000000u,
                     ((((pc >> 24) * (colorScale >> 24)) >> 8) << 24) | (((((pc >> 16) & 0xFF) * ((colorScale >> 16) & 0xFF)) >> 8) << 16) |
                     (((((pc >> 8) & 0xFF) * ((colorScale >> 8) & 0xFF)) >> 8) << 8) | (((pc & 0xFF) * (colorScale & 0xFF)) >> 8));
            f5_emit_rect(g, ulx, uly, lrx, lry, 0, (u16)S, (u16)T, dsdx, dtdy, 0);
        }
        vtxAddr += 0x100;
    }
}

// ---- Battle for Naboo
static void f5n_texrect_gen(H64Gfx *g)
{
    u32 cur = f5_cur(g), p[8], i, vi;
    const GfxVtx *v;
    float sx, sy, ulx, lrx, uly, lry, intpart;
    u32 wi, vp, pmX, pmY, offX, offY, p4X, p4Y;
    u64 p3X, p3Y;
    u16 dsdx, dtdy, sdsdx, sdtdy, E = 0, F = 0, S, T;
    int flip;
    for (i = 0; i < 8; i++) p[i] = rd32(g, cur + i * 4);
    vi = ((p[0] >> 5) & 0x07F8) / 40;
    if (vi >= GFX_VTX_MAX) return;
    v = &g->vtx[vi];
    if (v->clip & (CL_POSX | CL_NEGX | CL_POSY | CL_NEGY | CL_W)) return;
    sx = g->vtrans[0] + g->vscale[0] * v->x / v->w;
    sy = g->vtrans[1] - g->vscale[1] * v->y / v->w;
    flip = (p[0] & 1) != 0;
    wi = v->w >= 1.0f ? (u32)v->w : 1;
    vp = dmw(g, 0x130);
    pmX = (p[5] & 0xFFFF0000u) | (p[6] >> 16);
    pmY = (p[5] << 16) | (p[6] & 0xFFFF);
    p3X = p[3] >> 16;
    p3Y = p[3] & 0xFFFF;
    if (flip) { u64 k = p3X; p3X = p3Y; p3Y = k; }
    offX = (u32)(((p3X * ((vp >> 17) & 0x7FFF) * pmX) / wi) >> 16);
    offY = (u32)(((p3Y * ((vp >> 1) & 0x7FFF) * pmY) / wi) >> 16);
    ulx = sx - offX / 4.0f;
    lrx = sx + offX / 4.0f;
    uly = sy - offY / 4.0f;
    lry = sy + offY / 4.0f;
    if (!(lrx - ulx > 0.0f) || !(lry - uly > 0.0f)) return;
    update_combined(g);
    p4X = (p[4] & 0xFFFF0000u) | (u32)(fabsf(modff(g->combined[0][0], &intpart)) * 65536.0f);
    p4Y = (p[4] << 16) | (u32)(fabsf(modff(g->combined[0][1], &intpart)) * 65536.0f);
    if (flip) { u32 k = offX; offX = offY; offY = k; }
    if (!offX || !offY) return;
    dsdx = (u16)((p4X / offX) >> 10);
    dtdy = (u16)((p4Y / offY) >> 10);
    if (p[0] & 2) { dsdx = (u16)-dsdx; E = (u16)(p[4] >> 16); }
    if (p[0] & 4) { dtdy = (u16)-dtdy; F = (u16)p[4]; }
    sdsdx = dsdx; sdtdy = dtdy;
    if (flip) { u16 k = dsdx; dsdx = dtdy; dtdy = k; }
    S = ulx > 0 ? (u16)(E + 0xFFF0) : (u16)(((0 - ((int)dsdx << 6) * (int)(ulx * 4.0f)) << 3) + E + 0xFFF0);
    T = uly > 0 ? (u16)(F + 0xFFF0) : (u16)(((0 - ((int)dtdy << 6) * (int)(uly * 4.0f)) << 3) + F + 0xFFF0);
    f5i_sub_dl(g);
    f5_prim_depth(g, v);
    out_rdp1(g, 0xFA000000u, p[7]);
    if (geom(g) & GM_FOG) out_rdp1(g, 0xF8000000u, (p[7] & 0xFFFFFF00u) | (u32)v->a);
    f5_emit_rect(g, ulx, uly, lrx, lry, g->texTile, S, T, sdsdx, sdtdy, flip);
}

static void f5n_copy_ff(H64Gfx *g)
{
    u32 cur = f5_cur(g), p0 = rd32(g, cur), p1 = rd32(g, cur + 4), p2 = rd32(g, cur + 8), p3 = rd32(g, cur + 12), V;
    const u32 d = 0xDB8;
#define NW(k) (d + (k) * 4)
    dmw_set(g, 0x100, 0);
    dmw_set(g, NW(0), dmw(g, NW(0)) & 0xFFFF);
    dmw_set(g, NW(1), (p0 << 16) | (dmw(g, NW(1)) & 0xFFFF));
    V = (p0 >> 16) & 0xFF;
    dmw_set(g, NW(2), (V << 24) | 0x00FFFF);
    dmw_set(g, NW(3), V * 2);
    dmw_set(g, NW(4), V << 24);
    V = p1 >> 24;
    dmw_set(g, NW(5), V);
    dmw_set(g, NW(7), (dmw(g, NW(7)) & 0xFFFF0000u) | (V * 2));
    V = p2 >> 16;
    dmw_set(g, NW(10), (dmw(g, NW(10)) & 0xFFFF0000u) | V);
    dmw_set(g, NW(12), (dmw(g, NW(12)) & 0xFFFF0000u) | V);
    V = p2 & 0xFFFF;
    dmw_set(g, NW(14), (dmw(g, NW(14)) & 0xFFFF0000u) | V);
    dmw_set(g, NW(16), (dmw(g, NW(16)) & 0xFFFF0000u) | V);
    V = p3 >> 16;
    dmw_set(g, NW(18), (dmw(g, NW(18)) & 0xFFFF0000u) | V);
    dmw_set(g, NW(20), (dmw(g, NW(20)) & 0xFFFF0000u) | V);
    V = p3 & 0xFFFF;
    dmw_set(g, NW(22), (dmw(g, NW(22)) & 0xFFFF0000u) | V);
    dmw_set(g, NW(24), (dmw(g, NW(24)) & 0xFFFF0000u) | V);
    V = (p1 >> 8) & 0xFFFF;
    dmw_set(g, NW(27), (dmw(g, NW(27)) & 0xFFFF0000u) | V);
    dmw_set(g, NW(29), (dmw(g, NW(29)) & 0xFFFF0000u) | V);
#undef NW
}

static void f5n_copy_00(H64Gfx *g)
{
    static const u32 d58[14] = { 0x06000628, 0x06500678, 0x06A006C8, 0x06F00718, 0x07400768, 0x079007B8, 0x07E00808,
                                 0x08300858, 0x088008A8, 0x08D008F8, 0x09200948, 0x09700998, 0x09C009E8, 0x0A100A38 };
    u32 cur = f5_cur(g), p[16], V, i;
    const u32 d = 0xDD0;
#define NW(k) (d + (k) * 4)
#define RW(k) dmw(g, NW(k))
    for (i = 0; i < 16; i++) p[i] = rd32(g, cur + i * 4);
    dmw_set(g, 0x0F8, p[3]);
    V = (p[0] >> 16) & 0xFF;
    dmw_set(g, 0x0FC, (V << 24) | ((p[10] & 0xFF) << 16));
    dmw_set(g, NW(0), (p[2] & 0xFFFF0000u) | (p[14] >> 16));
    dmw_set(g, NW(1), (p[2] << 16) | (RW(1) & 0xFFFF));
    dmw_set(g, NW(2), (p[2] & 0xFFFF0000u) | (p[14] & 0xFFFF));
    dmw_set(g, NW(3), (p[2] << 16) | 0x0100);
    V = p[4] & 0xFFFF0000u;
    dmw_set(g, NW(4), V | (RW(4) & 0xFFFF));
    dmw_set(g, NW(6), V | (RW(6) & 0xFFFF));
    V = p[4] << 16;
    dmw_set(g, NW(8), V | (RW(8) & 0xFFFF));
    dmw_set(g, NW(10), V | (RW(10) & 0xFFFF));
    V = p[5] & 0xFFFF0000u;
    dmw_set(g, NW(12), V | (RW(12) & 0xFFFF));
    dmw_set(g, NW(14), V | (RW(14) & 0xFFFF));
    V = p[5] << 16;
    dmw_set(g, NW(16), V | (RW(16) & 0xFFFF));
    dmw_set(g, NW(18), V | (RW(18) & 0xFFFF));
    V = (p[6] & 0xFFFF0000u) | (p[8] >> 16);
    dmw_set(g, NW(5), V); dmw_set(g, NW(7), V);
    V = (p[6] << 16) | (p[8] & 0xFFFF);
    dmw_set(g, NW(9), V); dmw_set(g, NW(11), V);
    V = (p[7] & 0xFFFF0000u) | (p[9] >> 16);
    dmw_set(g, NW(13), V); dmw_set(g, NW(15), V);
    V = (p[7] << 16) | (p[9] & 0xFFFF);
    dmw_set(g, NW(17), V); dmw_set(g, NW(19), V);
    dmw_set(g, NW(20), (p[0] << 16) | (p[13] >> 16));
    dmw_set(g, NW(21), (p[15] & 0xFFFF0000u) | (RW(21) & 0xFFFF));
    {
        u32 v22 = p[11] & 0xFF;
        if (v22 > 127) v22 |= 0xFF00;
        V = ((p[0] & 0xFFFF) + 0x00FF) & 0xFF00;
        v22 |= V << 16;
        dmw_set(g, NW(22), v22);
    }
    dmw_set(g, NW(23), (p[15] << 16) | (RW(23) & 0xFFFF));
    dmw_set(g, NW(24), 0x7FFF7FFF);
    dmw_set(g, NW(25), p[12]);
    dmw_set(g, NW(26), 0x7FFF7FFF);
    dmw_set(g, NW(27), p[12]);
    dmw_set(g, NW(28), p[10]);
    dmw_set(g, NW(29), p[10]);
    dmw_set(g, NW(30), p[11]);
    dmw_set(g, NW(31), p[11]);
    dm_from_rdram(g, 0xC68, (p[1] >> 8) & 0xFFFFFF, 0xF0);
    *dmp(g, 0x37E ^ 3) = 0xFF;
    for (i = 0; i < 14; i++) dmw_set(g, 0xD58 + i * 4, d58[i]);
    dmw_set(g, 0xD90, 0x0A600000u | (dmw(g, 0xD90) & 0xFFFF));
#undef RW
#undef NW
}

static void f5n_prepare_draw(H64Gfx *g, const u32 *vt)
{
    u16 A;
    u8 B;
    u32 C, dl;
    f5n_restore(g);
    if (f5i_clipped(g, vt[0], vt[1], vt[2])) return;
    A = (u16)(dmw(g, 0x100) >> 8);
    B = dmb(g, 0x103 ^ 3);
    C = (u32)A | B;
    dmh_set(g, 0x100 + 2, (u16)C);
    dl = dmw(g, 0x58C) & 0x00FFFFFF;
    if (dl == 0)
    {
        if ((C << 8) != B)
        {
            if (B)
            {
                u32 h0 = g->omH, l0 = g->omL;   // a mode of this polygon's own (see below)
                f5i_dmem_cmds(g, 0xE50 + B);
                g->omH = h0;
                g->omL = l0;
                g->f5Restore = 1;
            }
            else f5i_dmem_cmds(g, 0xE50);
        }
        triangle(g, vt[0], vt[1], vt[2]);
        return;
    }
    f5i_sub_dl(g);
    {
        u32 h = 0xEF000000u | (g->omH & 0x00FFFFFF), E, G;
        dmw_set(g, 0xE50, h);
        dmw_set(g, 0xE54, g->omL);
        E = h & 0xFFCFFFFFu;
        dmw_set(g, 0xE70, E);
        dmw_set(g, 0xE60, E | 0x00100000u);
        G = dmb(g, 0x101 ^ 3);
        if (G != 0)
        {
            // This polygon's own mode goes to the RDP only: the microcode
            // keeps its mode, sent again before the next primitive.
            u32 h0 = g->omH, l0 = g->omL;
            f5i_dmem_cmds(g, 0xE50 + G);
            g->omH = h0;
            g->omL = l0;
            g->f5Restore = 1;
        }
    }
    triangle(g, vt[0], vt[1], vt[2]);
}

static void f5n_step3(H64Gfx *g, u32 GG)
{
    u32 idx[3], vt[3], alphaSum = 0, i;
    idx[0] = dmh(g, GG + 2 * (0x09 ^ 1)) + g->nCC;
    idx[1] = dmh(g, GG + 2 * (0x0A ^ 1)) + g->nCC;
    idx[2] = dmh(g, GG + 2 * (0x0B ^ 1)) + g->nCC;
    for (i = 0; i < 3; i++)
    {
        u32 va = dmh(g, 0xD58 + 2 * ((idx[i] / 2) ^ 1)), o = 0x0B78 + idx[i] * 2 + ((u16)g->nHH & 0xFFF);
        GfxVtx *v;
        vt[i] = (va - 0x600) / 40;
        if (vt[i] >= GFX_VTX_MAX) return;
        v = &g->vtx[vt[i]];
        v->r = s_u8f[dmb(g, o + 3)];
        v->g = s_u8f[dmb(g, o + 2)];
        v->b = s_u8f[dmb(g, o + 1)];
        if (!(geom(g) & GM_FOG)) v->a = s_u8f[dmb(g, o)];
        alphaSum += dmb(g, o);
    }
    if (g->nHH == 0) { dmw_set(g, 0x100, 0); f5n_prepare_draw(g, vt); return; }
    if (alphaSum == 0) { *dmp(g, 0x103 ^ 3) = 0x00; f5n_prepare_draw(g, vt); return; }
    if (g->nHH > 0) { *dmp(g, 0x103 ^ 3) = 0x10; f5n_prepare_draw(g, vt); return; }
    if (alphaSum != 0x02FD) { *dmp(g, 0x103 ^ 3) = 0x00; f5n_prepare_draw(g, vt); return; }
    {
        u8 al[3];
        u32 sum2 = 0;
        for (i = 0; i < 3; i++)
        {
            al[i] = dmb(g, 0xB00 + idx[i] * 2 + ((u16)g->nHH & 0xFFF) + (0x3 ^ 3));
            sum2 += al[i];
        }
        if (sum2 == 0)
        {
            g->vtx[vt[0]].a = s_u8f[al[0]];
            *dmp(g, 0x103 ^ 3) = 0x20;
        }
        else
        {
            *dmp(g, 0x103 ^ 3) = 0x10;
            for (i = 0; i < 3; i++) g->vtx[vt[i]].a = s_u8f[al[i]];
        }
        f5n_prepare_draw(g, vt);
    }
}

static void f5n_draw_polygons(H64Gfx *g)
{
    u32 cur = f5_cur(g), p0 = rd32(g, cur), step = 5, n = 0;
    if ((p0 >> 24) == 0xBE)
    {
        u32 p1 = rd32(g, cur + 4), GG, I1, I2, I3, I4, v1, v2, v3, v4;
        g->nHH = (s16)p0;
        GG = (u32)((s32)g->nEE - g->nDD);
        I1 = (dmh(g, GG + 2 * (0x09 ^ 1)) + g->nCC) >> 1;
        I2 = (dmh(g, GG + 2 * (0x0A ^ 1)) + g->nCC) >> 1;
        I3 = (dmh(g, GG + 2 * (0x0B ^ 1)) + g->nCC) >> 1;
        I4 = (dmh(g, GG + 2 * (0x0E ^ 1)) + g->nCC) >> 1;
        v1 = (dmh(g, 0xD58 + 2 * (I1 ^ 1)) - 0x600) / 40;
        v2 = (dmh(g, 0xD58 + 2 * (I2 ^ 1)) - 0x600) / 40;
        v3 = (dmh(g, 0xD58 + 2 * (I3 ^ 1)) - 0x600) / 40;
        v4 = (dmh(g, 0xD58 + 2 * (I4 ^ 1)) - 0x600) / 40;
        if (v1 < GFX_VTX_MAX && v2 < GFX_VTX_MAX && v3 < GFX_VTX_MAX && v4 < GFX_VTX_MAX)
        {
            // In s10.5 (GLideN64's texels * 32).
            g->vtx[v1].s = 0; g->vtx[v1].t = (float)(s16)p1;
            g->vtx[v2].s = (float)(s16)(p1 >> 16); g->vtx[v2].t = 0;
            g->vtx[v3].s = 0; g->vtx[v3].t = 0;
            g->vtx[v4].s = g->vtx[v2].s; g->vtx[v4].t = g->vtx[v1].t;
        }
        step = 3;
    }
    while (n++ < 4096)
    {
        switch (step)
        {
        case 3:
            f5n_step3(g, (u32)((s32)g->nEE - g->nDD));
            f5n_step3(g, (u32)((s32)g->nEE - g->nDD + 0x08));
            step = 4;
            break;
        case 4:
            if (g->nDD == 0x10) step = 6;
            else
            {
                g->nCC = (u8)(g->nCC + g->nTT);
                if (((g->nAA & 0xFF) | (g->nBB & 0xFF)) == 0) return;
                step = 5;
            }
            break;
        case 5:
            g->nDD = 0x10;
            if ((g->nAA & 0x80) == 0) { g->nAA <<= 1; step = 6; }
            else if ((g->nAA & 0x8000) == 0) { g->nAA <<= 1; step = 3; }
            else { g->nAA <<= 1; return; }
            break;
        default:
            g->nDD = 0;
            if ((g->nBB & 0x80) == 0) { g->nBB = (s16)(g->nBB << 1); step = 4; }
            else if ((g->nBB & 0x8000) == 0) { g->nBB = (s16)(g->nBB << 1); step = 3; }
            else { g->nBB = (s16)(g->nBB << 1); return; }
            break;
        }
    }
}

struct F5NVtx
{
    F5Sw vtx;
    u32 E;
    u16 K, L, M, N, I, J;
    u32 c1, c2;   // r g b a from the top byte
};

static s32 f5_mult_clip(s32 s, s32 t, s32 max)
{
    if (((u32)s & 0x80000000u) ^ ((u32)t & 0x80000000u)) return 0;
    {
        s64 r = ((s64)s * (s64)t) >> 32;
        return (s32)(r < (s64)max ? r : (s64)max);
    }
}

static u32 f5_rgba(u32 r, u32 gg, u32 b, u32 a) { return ((r & 0xFF) << 24) | ((gg & 0xFF) << 16) | ((b & 0xFF) << 8) | (a & 0xFF); }

// GENERATE (0x09 and 0x0C): terrain strips from height and colour data.
static void f5n_gen_vertices(H64Gfx *g, int mode0c)
{
    u32 cur = f5_cur(g), p[6], i;
    u32 src, dst, ra, src2, dst2, sh, flag, A, C32, C2, F, G, I, startAddr, endAddr, addrShift;
    s64 C64;
    s32 K, L, M, N, O, P, Q, R;
    u32 E20, E24, E28, E2C, DE0, DE4, DF0, DF4, E00, E04, E10, E14, E34, E40, E48, DD0, DD4, DD6, DD8;
    u8 RR, HH;
    F5NVtx vd[2];
    for (i = 0; i < 6; i++) p[i] = rd32(g, cur + i * 4);
    if (mode0c) dm_from_rdram(g, 0x380, dmw(g, 0x0F8) & 0xFFFFFF, 512);
    src = p[0] & 0xFFF;
    dst = dmw(g, src) >> 16;
    ra = p[4] & 0xFFFFFF;
    dm_from_rdram(g, dst, ra, 40);
    src2 = src + 8;
    dst2 = dmw(g, src2) >> 16;
    sh = dmw(g, 0xDBC) >> 16;
    dm_from_rdram(g, dst2, ra + sh, 40);
    flag = (p[0] >> 12) & 0xF;
    if (flag > 7)
    {
        u32 ra2 = p[5] & 0xFFFFFF;
        dm_from_rdram(g, dst + 0x78, ra2, 40);
        dm_from_rdram(g, dst2 + 0x78, ra2 + sh, 40);
    }
    A = (u32)((s32)(s8)(p[1] >> 24) * 0x10000);
    C32 = A - (dmw(g, 0xDD0) >> 16);
    C64 = (s64)(s32)C32;
    C2 = (u32)((C64 * C64) >> 16);
    F = dmw(g, 0xDCC) << 16;
    G = C32 * (dmw(g, 0xDC4) & 0xFFFF) + F;
    for (i = 0; i < 2; i++)
    {
        u32 V = i == 0 ? p[1] + (dmw(g, 0xDC0) >> 16) : p[1];
        u32 pa = i == 0 ? 0xDD0 : 0xDD8, B, D;
        s64 D64;
        vd[i].vtx.x = (s16)(((V >> 24) & 0xFF) << 8);
        vd[i].vtx.z = (s16)(((V >> 8) & 0xFF) << 8);
        vd[i].vtx.flag = (u16)((V & 0xFF) << 8);
        vd[i].vtx.y = 0;
        B = (u32)((s32)(s8)(V >> 8) * 0x10000);
        D = B - (dmw(g, pa + 4) >> 16);
        D64 = (s64)(s32)D;
        vd[i].E = C2 + (u32)((D64 * D64) >> 16);
    }
    E20 = dmw(g, 0xE20); E24 = dmw(g, 0xE24); E28 = dmw(g, 0xE28); E2C = dmw(g, 0xE2C);
    I = (E2C & 0xFFFF0000u) | (E24 >> 16);
    DE0 = dmw(g, 0xDE0); DE4 = dmw(g, 0xDE4); DF0 = dmw(g, 0xDF0); DF4 = dmw(g, 0xDF4);
    K = (s32)((DF0 & 0xFFFF0000u) | (DE0 >> 16));
    L = (s32)((DF0 << 16) | (DE0 & 0xFFFF));
    M = (s32)((DF4 & 0xFFFF0000u) | (DE4 >> 16));
    N = (s32)((DF4 << 16) | (DE4 & 0xFFFF));
    E00 = dmw(g, 0xE00); E04 = dmw(g, 0xE04); E10 = dmw(g, 0xE10); E14 = dmw(g, 0xE14);
    O = (s32)((E10 & 0xFFFF0000u) | (E00 >> 16));
    P = (s32)((E10 << 16) | (E00 & 0xFFFF));
    Q = (s32)((E14 & 0xFFFF0000u) | (E04 >> 16));
    R = (s32)((E14 << 16) | (E04 & 0xFFFF));
    E34 = dmw(g, 0xE34);
    startAddr = p[4] >> 24;
    endAddr = p[5] >> 24;
    E40 = dmw(g, 0xE40); E48 = dmw(g, 0xE48);
    DD0 = dmw(g, 0xDD0); DD4 = dmw(g, 0xDD4); DD6 = DD4 << 16; DD8 = dmw(g, 0xDD8);
    RR = dmb(g, 0x0FC ^ 3); HH = dmb(g, 0x0FD ^ 3);
    for (addrShift = startAddr; addrShift <= endAddr; addrShift += 4)
    {
        u32 addr[2];
        for (i = 0; i < 2; i++)
        {
            F5NVtx *d = &vd[i];
            u64 J;
            d->K = (u16)f5_mult_clip((s32)(K - (s32)d->E), O, 0x7FFF);
            d->L = (u16)f5_mult_clip((s32)(L - (s32)d->E), P, 0x7FFF);
            d->M = (u16)f5_mult_clip((s32)(M - (s32)d->E), Q, (s32)(E34 >> 16));
            d->N = (u16)f5_mult_clip((s32)(N - (s32)d->E), R, (s32)(E34 & 0xFFFF));
            J = ((u64)d->E * (u64)I) >> 16;
            J *= J;
            d->J = (u16)((u16)(J >> 47) < 0x7FFF ? (u16)(J >> 47) : 0x7FFF);
        }
        addr[0] = (dmw(g, src) >> 16) + addrShift;
        addr[1] = (dmw(g, src + 8) >> 16) + addrShift;
        for (i = 0; i < 2; i++)
        {
            F5NVtx *d = &vd[i];
            u32 a = addr[i];
            u32 cr, cg, cb, ca, c2r, c2g, c2b, c2a;
            if (mode0c)
            {
                u32 factor = 0xFFFE - d->M - d->N, U1 = dmw(g, a), U2 = dmw(g, a + 0x78), L1, L2, f3;
                u8 u1 = (u8)((U1 & 0xFF) + RR), u2 = (u8)((U2 & 0xFF) + HH), A1;
                u32 u3 = (u32)(s32)(s8)dmb(g, (0x380 + u1) ^ 3), u4 = (u32)(s32)(s8)dmb(g, (0x380 + u2) ^ 3);
                u16 u5, u6, u7, f1, f2;
                u8 Va, Vb, Vc;
                cr = (factor * (U1 >> 24) + d->M * (E40 >> 24) + d->N * (E48 >> 24)) >> 16;
                cg = (factor * ((U1 >> 16) & 0xFF) + d->M * ((E40 >> 16) & 0xFF) + d->N * ((E48 >> 16) & 0xFF)) >> 16;
                cb = (factor * ((U1 >> 8) & 0xFF) + d->M * ((E40 >> 8) & 0xFF) + d->N * ((E48 >> 8) & 0xFF)) >> 16;
                L1 = (E28 << 16) | (E20 & 0xFFFF);
                L2 = (DD8 << 16) | (DD0 & 0xFFFF);
                u5 = (u16)((u3 * L1 + u4 * L2) >> 16);
                u6 = (u16)((u5 + (E20 >> 16)) & 0xFFFF);
                u7 = (u16)(((0x7FFF - (u32)d->K) * u6 + (u32)d->K * (E28 >> 16)) >> 16);
                d->vtx.y = (s16)(((0x7FFF - (u32)d->J) * u7 + (u32)d->J * (E24 & 0xFFFF)) >> 16);
                A1 = dmb(g, (0x4FF + u4) ^ 3);
                f1 = (u16)(0xFF - A1);
                f2 = (u16)(0xFF * A1);
                Va = (u8)((f1 * (U2 >> 24) + f2) >> 8);
                Vb = (u8)((f1 * ((U2 >> 16) & 0xFF) + f2) >> 8);
                Vc = (u8)((f1 * ((U2 >> 8) & 0xFF) + f2) >> 8);
                c2r = (u8)((factor * Va + d->M * (E40 >> 24) + d->N * (E48 >> 24)) >> 16);
                c2g = (u8)((factor * Vb + d->M * ((E40 >> 16) & 0xFF) + d->N * ((E48 >> 16) & 0xFF)) >> 16);
                c2b = (u8)((factor * Vc + d->M * ((E40 >> 8) & 0xFF) + d->N * ((E48 >> 8) & 0xFF)) >> 16);
                cr &= 0xFF; cg &= 0xFF; cb &= 0xFF;
                f3 = 0x7FFF - d->K;
                c2r = (u8)((((f3 * c2r + d->K * cr) << 1) >> 16) + 1);
                c2g = (u8)((((f3 * c2g + d->K * cg) << 1) >> 16) + 1);
                c2b = (u8)((((f3 * c2b + d->K * cb) << 1) >> 16) + 1);
                ca = (u8)(((u32)d->K * d->K) >> 22);
                c2a = (u8)(((((0x7FFF - (u32)d->L) * A1 + (u32)d->L * 0xFF) << 1) >> 16) + 1);
            }
            else
            {
                u32 V = dmw(g, src + 0x20 + i * 4);
                u32 a3 = (V >> 16) + addrShift + (addrShift & 4), a4 = (V & 0xFFFF) + addrShift - (addrShift & 4);
                u32 f1 = 0x7FFF - d->L, f2 = 0xFFFE - d->M - d->N, U, T, TA, Vy1, Vy2, Vy, k, f3;
                u64 Va, Vb, Vc;
                u16 Y1, Y2;
                u32 pass, rr[2], gg2[2], bb[2];
                u16 yy[2];
                for (pass = 0; pass < 2; pass++)
                {
                    u32 o = pass ? 0x78 : 0;
                    u8 t[4];
                    U = dmw(g, a + o);
                    for (k = 0; k < 4; k++) t[k] = (u8)((dmb(g, a3 + o + k) + dmb(g, a4 + o + k)) >> 1);
                    T = t[0] | (u32)t[1] << 8 | (u32)t[2] << 16 | (u32)t[3] << 24;
                    Va = ((u64)(f1 * (U >> 24) + d->L * (T >> 24)) * f2) >> 16;
                    Vb = ((u64)(f1 * ((U >> 16) & 0xFF) + d->L * ((T >> 16) & 0xFF)) * f2) >> 16;
                    Vc = ((u64)(f1 * ((U >> 8) & 0xFF) + d->L * ((T >> 8) & 0xFF)) * f2) >> 16;
                    TA = ((dmw(g, a3 + o) & 0xFF) << 7) + ((dmw(g, a4 + o) & 0xFF) << 7);
                    Vy1 = (f1 * ((U & 0xFF) << 8)) >> 16;
                    Vy2 = (d->L * TA) >> 16;
                    Vy = Vy1 + Vy2;
                    rr[pass] = (u8)(((Va << 1) + d->M * (E40 >> 24) + d->N * (E48 >> 24)) >> 16);
                    gg2[pass] = (u8)(((Vb << 1) + d->M * ((E40 >> 16) & 0xFF) + d->N * ((E48 >> 16) & 0xFF)) >> 16);
                    bb[pass] = (u8)(((Vc << 1) + d->M * ((E40 >> 8) & 0xFF) + d->N * ((E48 >> 8) & 0xFF)) >> 16);
                    yy[pass] = (u16)((Vy * (0x7FFF - (u32)d->J) + (u32)d->J * (E24 & 0xFFFF)) >> 16);
                }
                Y1 = yy[0]; Y2 = yy[1];
                cr = rr[0]; cg = gg2[0]; cb = bb[0];
                f3 = 0x7FFF - d->K;
                c2r = (u8)((((f3 * rr[1] + d->K * cr) << 1) >> 16) + 1);
                c2g = (u8)((((f3 * gg2[1] + d->K * cg) << 1) >> 16) + 1);
                c2b = (u8)((((f3 * bb[1] + d->K * cb) << 1) >> 16) + 1);
                d->vtx.y = (s16)(((f3 * Y2 + (u32)d->K * Y1) << 1) >> 16);
                ca = c2a = (u8)(((u32)d->K * d->K) >> 22);
            }
            d->c1 = f5_rgba(cr, cg, cb, ca);
            d->c2 = f5_rgba(c2r, c2g, c2b, c2a);
            dmw_set(g, a + 0x0F0, d->c1);
            dmw_set(g, a + 0x168, d->c2);
        }
        sw_to_dm(g, 0x170 + addrShift * 2, &vd[0].vtx);
        sw_to_dm(g, 0x1C0 + addrShift * 2, &vd[1].vtx);
        {
            u32 V0 = dmw(g, 0xDC0), V1 = dmw(g, 0xDC4), V3 = dmw(g, 0xDCC);
            vd[0].vtx.x = (s16)(vd[0].vtx.x + (V0 >> 16));
            vd[0].vtx.flag = (u16)(vd[0].vtx.flag + (V1 & 0xFFFF));
            vd[1].vtx.x = (s16)(vd[1].vtx.x + (V0 >> 16));
            vd[1].vtx.flag = (u16)(vd[1].vtx.flag + (V3 & 0xFFFF));
        }
        vd[0].E += G;
        vd[1].E += G;
        if (!mode0c) DD6 = dmw(g, 0xDD4) << 16;
        G += DD6;
    }
    {
        u16 X0 = dmh(g, 0x128 + 2), Y0 = dmh(g, 0x128), Z0 = dmh(g, 0x128 + 6);
        int adj = (X0 | Y0 | Z0) != 0;
        u32 pass;
        for (pass = 0; pass < 2; pass++)
        {
            u32 prm = pass ? (p[2] & 0xFFFF) : (p[2] >> 16), num = prm & 0x1F, off, vsrc, vdst, k;
            if (!num) continue;
            off = prm >> 5;
            vsrc = (pass ? 0x1C0 : 0x170) + off;
            vdst = (dmw(g, src + (pass ? 0x18 : 0x10)) >> 16) + off * 5;
            for (k = 0; k < num; k++)
            {
                F5Sw v;
                sw_from_dm(g, vsrc + k * 8, &v);
                if (adj)
                {
                    v.x = (s16)(v.x + X0);
                    v.y = (s16)(v.y + Y0);
                    v.x = (s16)(v.x + Z0);   // as GLideN64 does (z added to x)
                }
                f5_vertex(g, (vdst - 0x600) / 40 + k, v.x, v.y, v.z);
            }
        }
    }
    g->nAA = (u16)(rd32(g, cur + 12) >> 16);
    g->nBB = (s16)(rd32(g, cur + 12) & 0xFFFF);
    g->nCC = rd8(g, cur + 1);
    g->nTT = rd8(g, cur + 5);
    g->nEE = (s16)src;
    g->nHH = (s16)(dmw(g, 0xDB8) >> 16);
    g->nDD = 0;
    f5n_draw_polygons(g);
}

static void f5i_geometry_gen(H64Gfx *g, u32 w0, u32 w1)
{
    switch (w1 & 0xFF)
    {
    case 0x09: f5n_gen_vertices(g, 0); g->pc[g->pci] += 16; break;
    case 0x0C: f5n_gen_vertices(g, 1); g->pc[g->pci] += 16; break;
    case 0x0F:
        if (dmb(g, 0x37E ^ 3) == 0) { f5n_copy_00(g); g->pc[g->pci] += 56; }
        else { f5n_copy_ff(g); g->pc[g->pci] += 8; }
        break;
    case 0x15: f5n_texrect_gen(g); g->pc[g->pci] += 24; break;
    case 0x18: f5i_gen_vertices(g, w0, w1); break;
    case 0x24: f5i_sub_dl(g); f5i_texrect_gen(g); g->pc[g->pci] += 32; break;
    case 0x27: f5i_gen_particles(g); g->pc[g->pci] += 16; break;
    case 0x4F: *dmp(g, 0x37E ^ 3) = 0; break;
    default: break;
    }
}

static void f5i_set_othermode(H64Gfx *g, u32 w0, u32 w1)
{
    u32 mask = (u32)((s32)0x80000000 / (1 << (w0 & 0x1F)));
    u32 a0 = (w0 >> 16) & 7;
    mask >>= (w0 >> 8) & 0x1F;
    if (a0 == 0) g->omH = (g->omH & ~mask) | w1;
    else if (a0 == 4) g->omL = (g->omL & ~mask) | w1;
    out_othermode(g);
}

static void run_f5indi(H64Gfx *g, u32 w0, u32 w1)
{
    u32 cmd = w0 >> 24;
    switch (cmd)
    {
    case 0x00: break;                                              // G_SPNOOP
    case 0x01: f5i_movemem(g, w0, w1); break;                      // G_MOVEMEM (DMA into DMEM, then its use)
    case 0x02:          // G_SET_DLIST_ADDR: the data stream G_MOVEMEM may read from
        dmw_set(g, 0xFD4, w1 + 8);
        dmw_set(g, 0xFD8, rd32(g, w1 & 0x00FFFFFF));
        dmw_set(g, 0xFDC, w0 & 0x1FF);
        break;
    case 0x05: f5i_geometry_gen(g, w0, w1); break;
    case 0x06:          // G_DL
        call_dl(g, w1, 1);
        if (!g->abort) f5_enter(g);
        break;
    case 0x07:          // G_BRANCH_DL
        call_dl(g, w1, 0);
        f5_enter(g);
        break;
    case 0xB4: f5i_tri(g, w0, w1, 1); break;                       // G_TRI2
    case 0xB5:          // G_JUMPDL: to the list's link word
        g->pc[g->pci] = g->f5dl[g->pci];
        f5_enter(g);
        break;
    case 0xB6: g->geomRaw &= w1; break;                            // G_CLEARGEOMETRYMODE (w1 is the mask kept)
    case 0xB7: g->geomRaw |= w1; break;                            // G_SETGEOMETRYMODE
    case 0xB8: end_dl(g); break;                                   // G_ENDDL
    case 0xB9:          // G_SETOTHERMODE_L, when its bit 23 matches DMEM 0x11C
        if (((w0 >> 23) & 1) == dmw(g, 0x11C)) f5i_set_othermode(g, w0, w1);
        break;
    case 0xBA: f5i_set_othermode(g, w0, w1); break;                // G_SETOTHERMODE_H (either half)
    case 0xBB:          // G_TEXTURE: also kept at 0x148, its bit 1 copied into the geometry mode
        set_texture(g, w1, (w0 >> 11) & 7, (w0 >> 8) & 7, w0 & 0xFF);
        dmw_set(g, 0x148, w0);
        g->geomRaw ^= (g->geomRaw ^ w0) & 2;
        break;
    case 0xBC:          // G_MOVEWORD: a word into DMEM, some of them used at once
    {
        u32 a = w0 & 0xFFF;
        dmw_set(g, a, w1);
        if (a == 0x04 || a == 0x0C || a == 0x14 || a == 0x1C)   // G_MWO_CLIP_R*: the clip ratio
        {
            int r = (s16)w1;
            if (r < 0) r = -r;
            if (r >= 1 && r <= 6) g->clipRatio = (float)r;
        }
        else if (a == 0x160) g->fogMul = (float)(s32)w1 / 256.0f;
        else if (a == 0x164) g->fogOff = (float)(s32)w1 / 256.0f;
        break;
    }
    case 0xBE: f5n_draw_polygons(g); break;                        // Naboo: textured polygons
    case 0xBF: f5i_tri(g, w0, w1, 0); break;                       // G_TRI1
    case 0xE4: case 0xE5:   // TEXRECT, after the sub display list; its second half follows
    {
        u32 a = g->pc[g->pci];
        u64 w[2];
        f5i_sub_dl(g);
        w[0] = ((u64)w0 << 32) | w1;
        w[1] = ((u64)rd32(g, a) << 32) | rd32(g, a + 4);
        g->pc[g->pci] += 8;
        out_rdp(g, w, 2);
        break;
    }
    default:
        if (cmd >= 0xC0) rdp_passthrough(g, w0, w1);
        else if (!(g->warned & 2)) { H64_WARN("[gfx] Factor 5 (Indiana Jones, Naboo) command %02X not handled", cmd); g->warned |= 2; }
        break;
    }
}
