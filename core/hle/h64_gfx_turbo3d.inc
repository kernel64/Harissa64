// Harissa64 V2 - Turbo3D (Dark Rift): included by h64_gfx.cpp.
//
// Studied in GLideN64 (uCodes/Turbo3D.cpp, GPL v2) and rewritten. The display
// list is a list of 16-byte entries: a global state, an object state, its
// vertices and its triangles (segment addresses; the list ends at an object
// state of 0). In RDRAM (big-endian):
// - global state: perspNorm (16 bits) and padding, flag, other modes (2 words),
//   16 segment bases, a viewport (16 bytes at 80), an RDP command list (at 96);
// - object state: render state (geometry mode bits, F3D's), texture state
//   (tile in the low 3 bits), vertex count, first vertex, triangle count, flag
//   (bytes 8..11), an RDP command list, other modes (2 words), then the
//   matrix (64 bytes) unless the flag is GT_FLAG_NOMTX;
// - triangles: 4 bytes, v0 v1 v2 and a flat-shade flag;
// - vertices: F3D's 16-byte vertices, or with GT_FLAG_NO_XFM vertices already
//   on screen (x, y in 10.2, z in 15.16, s, t, RGBA).
// The RDP command lists are raw RDP commands up to a pair of zero words.

#define T3D_FLAG_NOMTX 1
#define T3D_FLAG_NO_XFM 2

// Raw RDP commands (Turbo3D's state lists): sent as they are, a TEXRECT with
// its second 64-bit word, triangles with their attribute words.
static void t3d_rdp_list(H64Gfx *g, u32 addr)
{
    u32 a = seg_to_phys(g, addr), n = 0;
    if (!addr) return;
    for (;;)
    {
        u32 w0 = rd32(g, a), w1 = rd32(g, a + 4), op = w0 >> 24;
        if (w0 == 0 && w1 == 0) break;
        if (++n > 65536) { g->abort = 1; return; }   // no end: leave the task to the LLE RSP
        if (op == 0xE4 || op == 0xE5)
        {
            u64 w[2];
            w[0] = ((u64)w0 << 32) | w1;
            w[1] = ((u64)rd32(g, a + 8) << 32) | rd32(g, a + 12);
            out_rdp(g, w, 2);
            a += 16;
        }
        else if (op >= 0xC8 && op <= 0xCF)
        {
            u64 w[22];
            u32 k, words = 4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0);
            for (k = 0; k < words; k++) w[k] = ((u64)rd32(g, a + k * 8) << 32) | rd32(g, a + k * 8 + 4);
            out_rdp(g, w, words);
            a += words * 8;
        }
        else
        {
            if (op == 0xE9) g->fullSync = 1;
            if (op == 0xEF) { g->omH = w0 & 0x00FFFFFF; g->omL = w1; }
            if (op == 0xFD || op == 0xFE || op == 0xFF) w1 = seg_to_phys(g, w1);
            out_rdp1(g, w0, w1);
            a += 8;
        }
    }
}

// One entry of the display list.
static void t3d_step(H64Gfx *g)
{
    u32 a = g->pc[g->pci];
    u32 gstate = rd32(g, a), ostate = rd32(g, a + 4), pvtx = rd32(g, a + 8), ptri = rd32(g, a + 12);
    u32 o, flag, triCount, vtxV0, vtxCount, k;
    if (ostate == 0) { g->halt = 1; return; }
    g->pc[g->pci] = a + 16;
    if (gstate)
    {
        u32 s = seg_to_phys(g, gstate);
        g->omH = rd32(g, s + 8) & 0x00FFFFFF;
        g->omL = rd32(g, s + 12);
        out_othermode(g);
        for (k = 0; k < 16; k++) g->segments[k] = rd32(g, s + 16 + k * 4) & 0x00FFFFFF;
        set_viewport(g, gstate + 80);
        t3d_rdp_list(g, rd32(g, s + 96));
        if (g->abort) return;
    }
    o = seg_to_phys(g, ostate);
    vtxCount = rd8(g, o + 8);
    vtxV0 = rd8(g, o + 9);
    triCount = rd8(g, o + 10);
    flag = rd8(g, o + 11);
    g->texTile = rd32(g, o + 4) & 7;
    g->texLevel = 0;
    g->texOn = 1;
    g->texScaleS = g->texScaleT = 1.0f;
    g->omH = rd32(g, o + 16) & 0x00FFFFFF;
    g->omL = rd32(g, o + 20);
    out_othermode(g);
    if (flag != T3D_FLAG_NOMTX) force_matrix(g, ostate + 24);
    // F3D's geometry mode bits: lighting and fog off; smooth shading, shade,
    // z-buffer and back-face culling on.
    g->geomRaw = (rd32(g, o) & ~(0x20000u | 0x10000u)) | 0x200u | 0x4u | 0x1u | 0x2000u;
    if (vtxV0 + vtxCount > GFX_VTX_MAX) vtxCount = vtxV0 < GFX_VTX_MAX ? GFX_VTX_MAX - vtxV0 : 0;
    if (pvtx && flag != T3D_FLAG_NO_XFM && vtxCount) load_vertices(g, pvtx, vtxCount, vtxV0);
    t3d_rdp_list(g, rd32(g, o + 12));
    if (g->abort || !ptri) return;
    {
        u32 t = seg_to_phys(g, ptri);
        if (flag != T3D_FLAG_NO_XFM)
        {
            for (k = 0; k < triCount; k++, t += 4)
                triangle(g, rd8(g, t), rd8(g, t + 1), rd8(g, t + 2));
        }
        else if (pvtx)
        {
            // Vertices already on screen: drawn as they are.
            u32 v = seg_to_phys(g, pvtx), gm = geom(g), flags = 0;
            if (gm & GM_ZBUFFER) flags |= H64_TRI_ZBUFFER;
            if (gm & GM_SHADE) flags |= H64_TRI_SHADE;
            flags |= H64_TRI_TEXTURE;
            for (k = 0; k < triCount; k++, t += 4)
            {
                u32 idx[3], i;
                H64RenderVertex rv[3];
                idx[0] = rd8(g, t); idx[1] = rd8(g, t + 1); idx[2] = rd8(g, t + 2);
                if (idx[0] >= vtxCount || idx[1] >= vtxCount || idx[2] >= vtxCount) continue;
                for (i = 0; i < 3; i++)
                {
                    u32 p = v + idx[i] * 16;
                    rv[i].x = rd16(g, p) / 4.0f;
                    rv[i].y = rd16(g, p + 2) / 4.0f;
                    rv[i].z = (float)(s32)rd32(g, p + 4) / 65536.0f;
                    rv[i].invw = 1.0f;
                    rv[i].s = rd16(g, p + 8);
                    rv[i].t = rd16(g, p + 10);
                    rv[i].r = rd8(g, p + 12);
                    rv[i].g = rd8(g, p + 13);
                    rv[i].b = rd8(g, p + 14);
                    rv[i].a = rd8(g, p + 15);
                }
                out_tri(g, &rv[0], &rv[1], &rv[2], flags);
                g->nTris++;
            }
        }
    }
}
