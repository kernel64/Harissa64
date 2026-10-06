// Harissa64 V2 - RDP triangle setup: builds the RDP triangle command
// (edge, shade, texture and depth coefficients) from three screen-space
// vertices, as the graphics microcode does on the RSP.
//
// Port of libdragon's CPU reference setup (src/rdpq/rdpq_tri.c,
// rdpq_triangle_cpu and the __rdpq_write_*_coeffs helpers, commit e356bf3,
// public domain / Unlicense, see THIRD_PARTY.md), in double precision, with
// the inputs in RDP units (see render/api.h).
#include "h64_rdp.h"

#include <math.h>

#include "../../render/api.h"

static s32 to_s16_16(double f)
{
    if (f >= 32768.0) return 0x7FFFFFFF;
    if (f < -32768.0) return (s32)0x80000000;
    return (s32)floor(f * 65536.0);
}

static u64 pack(u32 hi, u32 lo) { return ((u64)hi << 32) | lo; }

struct EdgeData
{
    double hx, hy, mx, my;   // high (1->3) and middle (1->2) edge deltas
    double fy;               // floor(y1) - y1
    double ish;              // inverse slope of the high edge
    double attrFactor;       // -1 / (cross product): turns deltas into gradients
};

// Gradients of one attribute: d/dx, d/dy, d/de (along the high edge), and
// its value at the top scanline.
static void gradients(const EdgeData *e, double a1, double a2, double a3, double *start, double *ddx, double *ddy,
                      double *dde)
{
    double ma = a2 - a1, ha = a3 - a1;
    double nx = e->hy * ma - e->my * ha;
    double ny = e->mx * ha - e->hx * ma;
    *ddx = nx * e->attrFactor;
    *ddy = ny * e->attrFactor;
    *dde = *ddy + *ddx * e->ish;
    *start = a1 + e->fy * *dde;
}

u32 h64_rdp_build_triangle(const H64RenderVertex *a, const H64RenderVertex *b, const H64RenderVertex *c, u32 flags,
                           u32 tile, u32 levels, u64 *out)
{
    const H64RenderVertex *v1 = a, *v2 = b, *v3 = c, *tmp;
    EdgeData e;
    u32 n = 0;

    // Sort by y (top first).
    if (v1->y > v2->y) { tmp = v1; v1 = v2; v2 = tmp; }
    if (v2->y > v3->y) { tmp = v2; v2 = v3; v3 = tmp; }
    if (v1->y > v2->y) { tmp = v1; v1 = v2; v2 = tmp; }

    // ---- Edges ----
    {
        double x1 = v1->x, x2 = v2->x, x3 = v3->x;
        double y1 = floor(v1->y * 4.0) / 4.0, y2 = floor(v2->y * 4.0) / 4.0, y3 = floor(v3->y * 4.0) / 4.0;
        s32 y1f = (s32)floor(v1->y * 4.0), y2f = (s32)floor(v2->y * 4.0), y3f = (s32)floor(v3->y * 4.0);
        double lx, ly, nz, ism, isl, xh, xm, xl;
        u32 lft;
        if (y1f < -4096 * 4) y1f = -4096 * 4;
        if (y1f > 4095 * 4) y1f = 4095 * 4;
        if (y2f < -4096 * 4) y2f = -4096 * 4;
        if (y2f > 4095 * 4) y2f = 4095 * 4;
        if (y3f < -4096 * 4) y3f = -4096 * 4;
        if (y3f > 4095 * 4) y3f = 4095 * 4;

        e.hx = x3 - x1;
        e.hy = y3 - y1;
        e.mx = x2 - x1;
        e.my = y2 - y1;
        lx = x3 - x2;
        ly = y3 - y2;
        nz = e.hx * e.my - e.hy * e.mx;
        e.attrFactor = fabs(nz) > 1e-30 ? -1.0 / nz : 0;
        lft = nz < 0;
        e.ish = fabs(e.hy) > 1e-30 ? e.hx / e.hy : 0;
        ism = fabs(e.my) > 1e-30 ? e.mx / e.my : 0;
        isl = fabs(ly) > 1e-30 ? lx / ly : 0;
        e.fy = floor(y1) - y1;
        xh = x1 + e.fy * e.ish;
        xm = x1 + e.fy * ism;
        xl = x2;

        out[n++] = pack(((0x08u | (flags & 7)) << 24) | (lft << 23) | (((levels ? levels - 1 : 0) & 7) << 19) |
                            ((tile & 7) << 16) | ((u32)y3f & 0x3FFF),
                        (((u32)y2f & 0x3FFF) << 16) | ((u32)y1f & 0x3FFF));
        out[n++] = pack((u32)to_s16_16(xl), (u32)to_s16_16(isl));
        out[n++] = pack((u32)to_s16_16(xh), (u32)to_s16_16(e.ish));
        out[n++] = pack((u32)to_s16_16(xm), (u32)to_s16_16(ism));
    }

    // ---- Shade ----
    if (flags & H64_TRI_SHADE)
    {
        double c1[4] = { v1->r, v1->g, v1->b, v1->a };
        double c2[4] = { v2->r, v2->g, v2->b, v2->a };
        double c3[4] = { v3->r, v3->g, v3->b, v3->a };
        s32 fin[4], dx[4], dy[4], de[4];
        int i;
        for (i = 0; i < 4; i++)
        {
            double st, ddx, ddy, dde;
            gradients(&e, c1[i], c2[i], c3[i], &st, &ddx, &ddy, &dde);
            fin[i] = to_s16_16(st);
            dx[i] = to_s16_16(ddx);
            dy[i] = to_s16_16(ddy);
            de[i] = to_s16_16(dde);
        }
        out[n++] = pack(((u32)fin[0] & 0xFFFF0000u) | (((u32)fin[1] >> 16) & 0xFFFF),
                        ((u32)fin[2] & 0xFFFF0000u) | (((u32)fin[3] >> 16) & 0xFFFF));
        out[n++] = pack(((u32)dx[0] & 0xFFFF0000u) | (((u32)dx[1] >> 16) & 0xFFFF),
                        ((u32)dx[2] & 0xFFFF0000u) | (((u32)dx[3] >> 16) & 0xFFFF));
        out[n++] = pack(((u32)fin[0] << 16) | ((u32)fin[1] & 0xFFFF), ((u32)fin[2] << 16) | ((u32)fin[3] & 0xFFFF));
        out[n++] = pack(((u32)dx[0] << 16) | ((u32)dx[1] & 0xFFFF), ((u32)dx[2] << 16) | ((u32)dx[3] & 0xFFFF));
        out[n++] = pack(((u32)de[0] & 0xFFFF0000u) | (((u32)de[1] >> 16) & 0xFFFF),
                        ((u32)de[2] & 0xFFFF0000u) | (((u32)de[3] >> 16) & 0xFFFF));
        out[n++] = pack(((u32)dy[0] & 0xFFFF0000u) | (((u32)dy[1] >> 16) & 0xFFFF),
                        ((u32)dy[2] & 0xFFFF0000u) | (((u32)dy[3] >> 16) & 0xFFFF));
        out[n++] = pack(((u32)de[0] << 16) | ((u32)de[1] & 0xFFFF), ((u32)de[2] << 16) | ((u32)de[3] & 0xFFFF));
        out[n++] = pack(((u32)dy[0] << 16) | ((u32)dy[1] & 0xFFFF), ((u32)dy[2] << 16) | ((u32)dy[3] & 0xFFFF));
    }

    // ---- Texture (s, t premultiplied by the normalised 1/w) ----
    if (flags & H64_TRI_TEXTURE)
    {
        double w1 = v1->invw, w2 = v2->invw, w3 = v3->invw, maxw = w1, minw;
        double a1[3], a2[3], a3[3];
        s32 fin[3], dx[3], dy[3], de[3];
        int i;
        if (w2 > maxw) maxw = w2;
        if (w3 > maxw) maxw = w3;
        minw = maxw > 0 ? 1.0 / maxw : 1.0;
        w1 *= minw;
        w2 *= minw;
        w3 *= minw;
        a1[0] = v1->s * w1; a1[1] = v1->t * w1; a1[2] = w1 * 0x7FFF;
        a2[0] = v2->s * w2; a2[1] = v2->t * w2; a2[2] = w2 * 0x7FFF;
        a3[0] = v3->s * w3; a3[1] = v3->t * w3; a3[2] = w3 * 0x7FFF;
        for (i = 0; i < 3; i++)
        {
            double st, ddx, ddy, dde;
            gradients(&e, a1[i], a2[i], a3[i], &st, &ddx, &ddy, &dde);
            fin[i] = to_s16_16(st);
            dx[i] = to_s16_16(ddx);
            dy[i] = to_s16_16(ddy);
            de[i] = to_s16_16(dde);
        }
        out[n++] = pack(((u32)fin[0] & 0xFFFF0000u) | (((u32)fin[1] >> 16) & 0xFFFF), (u32)fin[2] & 0xFFFF0000u);
        out[n++] = pack(((u32)dx[0] & 0xFFFF0000u) | (((u32)dx[1] >> 16) & 0xFFFF), (u32)dx[2] & 0xFFFF0000u);
        out[n++] = pack(((u32)fin[0] << 16) | ((u32)fin[1] & 0xFFFF), (u32)fin[2] << 16);
        out[n++] = pack(((u32)dx[0] << 16) | ((u32)dx[1] & 0xFFFF), (u32)dx[2] << 16);
        out[n++] = pack(((u32)de[0] & 0xFFFF0000u) | (((u32)de[1] >> 16) & 0xFFFF), (u32)de[2] & 0xFFFF0000u);
        out[n++] = pack(((u32)dy[0] & 0xFFFF0000u) | (((u32)dy[1] >> 16) & 0xFFFF), (u32)dy[2] & 0xFFFF0000u);
        out[n++] = pack(((u32)de[0] << 16) | ((u32)de[1] & 0xFFFF), (u32)de[2] << 16);
        out[n++] = pack(((u32)dy[0] << 16) | ((u32)dy[1] & 0xFFFF), (u32)dy[2] << 16);
    }

    // ---- Depth ----
    if (flags & H64_TRI_ZBUFFER)
    {
        double st, ddx, ddy, dde;
        gradients(&e, v1->z, v2->z, v3->z, &st, &ddx, &ddy, &dde);
        out[n++] = pack((u32)to_s16_16(st), (u32)to_s16_16(ddx));
        out[n++] = pack((u32)to_s16_16(dde), (u32)to_s16_16(ddy));
    }
    return n;
}
