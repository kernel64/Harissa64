// RDP triangle setup (h64_rdp_build_triangle) and its inverse
// (h64_rdp_decode_triangle, used by GPU renderers for LLE graphics): a
// triangle built then decoded gives back its vertices.
#include "unit_tests.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../core/rdp/h64_rdp.h"
#include "../../render/api.h"

static void set_vertex(H64RenderVertex *v, float x, float y, float z, float invw, float r, float g, float b, float a,
                       float s, float t)
{
    v->x = x; v->y = y; v->z = z; v->invw = invw;
    v->r = r; v->g = g; v->b = b; v->a = a;
    v->s = s; v->t = t;
}

static int near_value(double a, double b, double tol) { return fabs(a - b) <= tol; }

static void round_trip(H64TestContext *ctx, int persp)
{
    H64RenderVertex in[3], out[3];
    u64 cmd[22];
    u32 w[44], n, i;
    // Sorted top to bottom, as the decoder returns them; y on quarter pixels.
    set_vertex(&in[0], 100.3f, 20.25f, 1000.0f, persp ? 0.5f : 1.0f, 200, 10, 30, 255, 64.0f, 32.0f);
    set_vertex(&in[1], 40.7f, 90.5f, 2000.0f, persp ? 0.25f : 1.0f, 20, 220, 40, 128, 512.0f, 96.0f);
    set_vertex(&in[2], 180.1f, 150.75f, 3000.0f, persp ? 0.125f : 1.0f, 10, 20, 250, 0, 300.0f, 800.0f);
    n = h64_rdp_build_triangle(&in[0], &in[1], &in[2], H64_TRI_SHADE | H64_TRI_TEXTURE | H64_TRI_ZBUFFER, 0, 1, cmd);
    H64_CHECK_EQ(ctx, n, 22u);
    for (i = 0; i < n; i++)
    {
        w[i * 2] = (u32)(cmd[i] >> 32);
        w[i * 2 + 1] = (u32)cmd[i];
    }
    h64_rdp_decode_triangle(w, persp, out);
    for (i = 0; i < 3; i++)
    {
        H64_CHECK(ctx, near_value(out[i].x, in[i].x, 0.01));
        H64_CHECK(ctx, near_value(out[i].y, in[i].y, 0.01));
        H64_CHECK(ctx, near_value(out[i].z, in[i].z, 0.5));
        H64_CHECK(ctx, near_value(out[i].r, in[i].r, 0.5));
        H64_CHECK(ctx, near_value(out[i].g, in[i].g, 0.5));
        H64_CHECK(ctx, near_value(out[i].b, in[i].b, 0.5));
        H64_CHECK(ctx, near_value(out[i].a, in[i].a, 0.5));
        H64_CHECK(ctx, near_value(out[i].s, in[i].s, 0.5));
        H64_CHECK(ctx, near_value(out[i].t, in[i].t, 0.5));
        if (persp)   // 1/w comes back normalised: compare ratios
            H64_CHECK(ctx, near_value(out[i].invw / out[0].invw, in[i].invw / in[0].invw, 0.001));
    }
}

static double tri_area(const H64RenderVertex *v)
{
    return fabs((v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y)) / 2.0;
}

// h64_rdp_decode_polygon: a triangle comes back as triangles of the same
// area and attributes; a trapezoid (H and M edges apart at the top, as
// GoldenEye's sky) as its whole area.
static void polygon(H64TestContext *ctx)
{
    H64RenderVertex in[3], out[12];
    u64 cmd[22];
    u32 w[44], n, i;
    double area = 0;
    set_vertex(&in[0], 100.3f, 20.25f, 1000.0f, 1.0f, 200, 10, 30, 255, 64.0f, 32.0f);
    set_vertex(&in[1], 40.7f, 90.5f, 2000.0f, 1.0f, 20, 220, 40, 128, 512.0f, 96.0f);
    set_vertex(&in[2], 180.1f, 150.75f, 3000.0f, 1.0f, 10, 20, 250, 0, 300.0f, 800.0f);
    n = h64_rdp_build_triangle(&in[0], &in[1], &in[2], H64_TRI_SHADE | H64_TRI_TEXTURE | H64_TRI_ZBUFFER, 0, 1, cmd);
    for (i = 0; i < n; i++)
    {
        w[i * 2] = (u32)(cmd[i] >> 32);
        w[i * 2 + 1] = (u32)cmd[i];
    }
    n = h64_rdp_decode_polygon(w, 0, out);
    H64_CHECK(ctx, n >= 1 && n <= 4);
    for (i = 0; i < n; i++) area += tri_area(&out[3 * i]);
    H64_CHECK(ctx, near_value(area, tri_area(in), 1.0));
    // The first vertex is the input's top vertex, with its attributes.
    H64_CHECK(ctx, near_value(out[0].x, in[0].x, 0.01) && near_value(out[0].y, in[0].y, 0.01));
    H64_CHECK(ctx, near_value(out[0].r, in[0].r, 0.5));
    // A trapezoid: H edge x = 10 (vertical), M edge x = 200, rows 8 to 40, no attributes.
    memset(w, 0, sizeof(w));
    w[0] = (0x08u << 24) | (160u);        // fill triangle, YL = 40.0
    w[1] = (160u << 16) | 32u;             // YM = 40.0, YH = 8.0
    w[2] = 200u << 16;                     // XL
    w[4] = 10u << 16;                      // XH
    w[6] = 200u << 16;                     // XM
    n = h64_rdp_decode_polygon(w, 0, out);
    area = 0;
    for (i = 0; i < n; i++) area += tri_area(&out[3 * i]);
    H64_CHECK(ctx, near_value(area, 190.0 * 32.0, 1.0));
}

void test_rdp_tri(H64TestContext *ctx)
{
    round_trip(ctx, 0);
    round_trip(ctx, 1);
    polygon(ctx);
}
