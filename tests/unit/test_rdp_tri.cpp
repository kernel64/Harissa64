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

void test_rdp_tri(H64TestContext *ctx)
{
    round_trip(ctx, 0);
    round_trip(ctx, 1);
}
