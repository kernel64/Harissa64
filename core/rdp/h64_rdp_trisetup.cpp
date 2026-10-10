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
#include <string.h>

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
        // Largest 1/w scaled to 0x7FFF, or to 0x7000 when W varies: the W sent
        // is extrapolated to the scanline above the top vertex and must not
        // wrap past 0x7FFF (libdragon always scales to 0x7FFF). The RDP's S/W
        // is unchanged; with a constant W (2D, or perspective correction
        // off, where the RDP reads S directly) S stays the coordinate itself.
        minw = maxw > 0 ? 1.0 / maxw : 1.0;
        if (w1 != w2 || w2 != w3) minw *= (double)0x7000 / 0x7FFF;
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

static s32 sext(s32 v, int bits) { return (s32)((u32)v << (32 - bits)) >> (32 - bits); }

// The inverse of h64_rdp_build_triangle: rebuilds the three vertices of an
// RDP triangle command from its edges and attribute gradients.
// Attributes are given on the major edge at the top scanline (integer part
// of YH): a(x, y) = A + DaDe (y - y0) + DaDx (x - xh(y)).
static double fix16(u32 hi, u32 lo) { return (double)(s32)((hi & 0xFFFF0000u) | (lo >> 16)) / 65536.0; }
static double fix16lo(u32 hi, u32 lo) { return (double)(s32)((hi << 16) | (lo & 0xFFFF)) / 65536.0; }

// The attributes of the command at screen position (px, py).
static void attr_at(const u32 *w, int persp, double px, double py, H64RenderVertex *o)
{
    u32 op = (w[0] >> 24) & 0x3F;
    double yh = sext((s32)(w[1] & 0x3FFF), 14) / 4.0, y0 = floor(yh);
    double xh = (s32)w[4] / 65536.0, dxh = (s32)w[5] / 65536.0;
    double ey = py - y0, ex = px - (xh + dxh * ey), a[8];
    const u32 *p = w + 8;
    int k;
    memset(a, 0, sizeof(a));
    if (op & 4)
    {
        for (k = 0; k < 4; k++)
        {
            u32 wi = k < 2 ? 0 : 1, hiHalf = (k & 1) == 0;
            double val = hiHalf ? fix16(p[wi], p[4 + wi]) : fix16lo(p[wi], p[4 + wi]);
            double ddx = hiHalf ? fix16(p[2 + wi], p[6 + wi]) : fix16lo(p[2 + wi], p[6 + wi]);
            double dde = hiHalf ? fix16(p[8 + wi], p[12 + wi]) : fix16lo(p[8 + wi], p[12 + wi]);
            a[k] = val + dde * ey + ddx * ex;
        }
        p += 16;
    }
    if (op & 2)
    {
        for (k = 0; k < 3; k++)
        {
            u32 wi = k < 2 ? 0 : 1, hiHalf = (k & 1) == 0;
            double val = hiHalf ? fix16(p[wi], p[4 + wi]) : fix16lo(p[wi], p[4 + wi]);
            double ddx = hiHalf ? fix16(p[2 + wi], p[6 + wi]) : fix16lo(p[2 + wi], p[6 + wi]);
            double dde = hiHalf ? fix16(p[8 + wi], p[12 + wi]) : fix16lo(p[8 + wi], p[12 + wi]);
            a[4 + k] = val + dde * ey + ddx * ex;
        }
        p += 16;
    }
    if (op & 1) a[7] = (s32)p[0] / 65536.0 + (s32)p[2] / 65536.0 * ey + (s32)p[1] / 65536.0 * ex;
    o->x = (float)px;
    o->y = (float)py;
    o->z = (float)a[7];
    o->r = (float)a[0];
    o->g = (float)a[1];
    o->b = (float)a[2];
    o->a = (float)a[3];
    if (persp)
    {
        double ww = a[6] > 1.0 ? a[6] : 1.0;
        o->s = (float)(a[4] * 32767.0 / ww);
        o->t = (float)(a[5] * 32767.0 / ww);
        o->invw = (float)(ww / 32767.0);
    }
    else
    {
        o->s = (float)a[4];
        o->t = (float)a[5];
        o->invw = 1.0f;
    }
}

u32 h64_rdp_decode_polygon(const u32 *w, int persp, H64RenderVertex *v)
{
    double yl = sext((s32)(w[0] & 0x3FFF), 14) / 4.0, ym = sext((s32)((w[1] >> 16) & 0x3FFF), 14) / 4.0;
    double yh = sext((s32)(w[1] & 0x3FFF), 14) / 4.0, y0 = floor(yh);
    double xl = (s32)w[2] / 65536.0, dxl = (s32)w[3] / 65536.0;
    double xh = (s32)w[4] / 65536.0, dxh = (s32)w[5] / 65536.0;
    double xm = (s32)w[6] / 65536.0, dxm = (s32)w[7] / 65536.0;
    double px[12], py[12];
    u32 n = 0, t;
    if (ym < yh) ym = yh;
    if (ym > yl) ym = yl;
    // Two trapezoids, each corners H top, other side top, other side bottom, H bottom.
    for (t = 0; t < 2; t++)
    {
        double ya = t ? ym : yh, yb = t ? yl : ym, q[4][2];
        int k;
        if (yb <= ya) continue;
        q[0][0] = xh + dxh * (ya - y0); q[0][1] = ya;
        q[3][0] = xh + dxh * (yb - y0); q[3][1] = yb;
        if (!t) { q[1][0] = xm + dxm * (ya - y0); q[2][0] = xm + dxm * (yb - y0); }
        else { q[1][0] = xl; q[2][0] = xl + dxl * (yb - ym); }
        q[1][1] = ya;
        q[2][1] = yb;
        // (0 1 2) and (0 2 3), unless one has no width.
        for (k = 0; k < 2; k++)
        {
            int a = 0, b = k ? 2 : 1, c = k ? 3 : 2;
            double area = (q[b][0] - q[a][0]) * (q[c][1] - q[a][1]) - (q[c][0] - q[a][0]) * (q[b][1] - q[a][1]);
            if (fabs(area) < 1e-6) continue;
            px[n] = q[a][0]; py[n] = q[a][1]; n++;
            px[n] = q[b][0]; py[n] = q[b][1]; n++;
            px[n] = q[c][0]; py[n] = q[c][1]; n++;
        }
    }
    for (t = 0; t < n; t++) attr_at(w, persp, px[t], py[t], &v[t]);
    return n / 3;
}

void h64_rdp_decode_triangle(const u32 *w, int persp, H64RenderVertex *v)
{
    u32 op = (w[0] >> 24) & 0x3F;
    double yl = sext((s32)(w[0] & 0x3FFF), 14) / 4.0, ym = sext((s32)((w[1] >> 16) & 0x3FFF), 14) / 4.0;
    double yh = sext((s32)(w[1] & 0x3FFF), 14) / 4.0;
    double xl = (s32)w[2] / 65536.0, xh = (s32)w[4] / 65536.0, dxh = (s32)w[5] / 65536.0;
    double y0 = floor(yh), vx[3], vy[3];
    double attr[3][8];   // r g b a, s t w, z
    const u32 *p = w + 8;
    int i, k;
    vy[0] = yh; vx[0] = xh + dxh * (yh - y0);
    vy[1] = ym; vx[1] = xl;
    vy[2] = yl; vx[2] = xh + dxh * (yl - y0);
    memset(attr, 0, sizeof(attr));
    for (i = 0; i < 3; i++)
    {
        double ex = vx[i] - (xh + dxh * (vy[i] - y0)), ey = vy[i] - y0;
        if (op & 4)
        {
            // r g b a: value, d/dx, d/de, d/dy as (int, frac) halves.
            for (k = 0; k < 4; k++)
            {
                u32 wi = k < 2 ? 0 : 1, hiHalf = (k & 1) == 0;
                double val = hiHalf ? fix16(p[wi], p[4 + wi]) : fix16lo(p[wi], p[4 + wi]);
                double ddx = hiHalf ? fix16(p[2 + wi], p[6 + wi]) : fix16lo(p[2 + wi], p[6 + wi]);
                double dde = hiHalf ? fix16(p[8 + wi], p[12 + wi]) : fix16lo(p[8 + wi], p[12 + wi]);
                attr[i][k] = val + dde * ey + ddx * ex;
            }
        }
    }
    if (op & 4) p += 16;
    if (op & 2)
    {
        for (i = 0; i < 3; i++)
        {
            double ex = vx[i] - (xh + dxh * (vy[i] - y0)), ey = vy[i] - y0;
            for (k = 0; k < 3; k++)
            {
                u32 wi = k < 2 ? 0 : 1, hiHalf = (k & 1) == 0;
                double val = hiHalf ? fix16(p[wi], p[4 + wi]) : fix16lo(p[wi], p[4 + wi]);
                double ddx = hiHalf ? fix16(p[2 + wi], p[6 + wi]) : fix16lo(p[2 + wi], p[6 + wi]);
                double dde = hiHalf ? fix16(p[8 + wi], p[12 + wi]) : fix16lo(p[8 + wi], p[12 + wi]);
                attr[i][4 + k] = val + dde * ey + ddx * ex;
            }
        }
        p += 16;
    }
    if (op & 1)
    {
        double z = (s32)p[0] / 65536.0, dzdx = (s32)p[1] / 65536.0, dzde = (s32)p[2] / 65536.0;
        for (i = 0; i < 3; i++)
            attr[i][7] = z + dzde * (vy[i] - y0) + dzdx * (vx[i] - (xh + dxh * (vy[i] - y0)));
    }
    for (i = 0; i < 3; i++)
    {
        double ww = attr[i][6] > 1.0 ? attr[i][6] : 1.0;
        v[i].x = (float)vx[i];
        v[i].y = (float)vy[i];
        v[i].z = (float)attr[i][7];
        v[i].r = (float)attr[i][0];
        v[i].g = (float)attr[i][1];
        v[i].b = (float)attr[i][2];
        v[i].a = (float)attr[i][3];
        if (persp)
        {
            v[i].s = (float)(attr[i][4] * 32767.0 / ww);
            v[i].t = (float)(attr[i][5] * 32767.0 / ww);
            v[i].invw = (float)(ww / 32767.0);
        }
        else
        {
            v[i].s = (float)attr[i][4];
            v[i].t = (float)attr[i][5];
            v[i].invw = 1.0f;
        }
    }
}

