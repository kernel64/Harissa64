// h64_rdp_fill8_fast (a fill rectangle into an 8-bit colour image written
// directly) against the software RDP's own fill: the same bytes and hidden
// bits, for rectangles and scissors cutting them in various ways.
#include "unit_tests.h"

#include <stdlib.h>
#include <string.h>

#include "../../core/rdp/h64_rdp.h"
#include "../../core/rdp/h64_rdp_state.h"
#include "../../core/system/h64_system.h"

static const u32 IMG = 0x2001;   // odd on purpose: the fill byte follows the RDRAM address
static const u32 WIDTH = 304;

static H64System *make_fill_sys(void)
{
    H64System *sys = (H64System *)calloc(1, sizeof(H64System));
    sys->rdram = (u8 *)calloc(1, H64_RDRAM_SIZE);
    return sys;
}

static void free_fill_sys(H64System *sys)
{
    h64_rdp_free(sys);
    free(sys->rdram);
    free(sys);
}

static void fill_cmd(H64System *sys, u32 w0, u32 w1)
{
    u64 w = (u64)w0 << 32 | w1;
    h64_rdp_command(sys, &w, 1);
}

// Fill mode, an 8-bit colour image, a scissor and a fill colour.
static void fill_setup(H64System *sys, u32 sx0, u32 sy0, u32 sx1, u32 sy1, u32 color)
{
    fill_cmd(sys, (0x2Fu << 24) | (3u << 20), 0);                                   // cycle type: fill
    fill_cmd(sys, (0x3Fu << 24) | (0u << 21) | (1u << 19) | (WIDTH - 1), IMG);       // I8 colour image
    fill_cmd(sys, (0x2Du << 24) | (sx0 << 12) | sy0, (sx1 << 12) | sy1);            // scissor (10.2)
    fill_cmd(sys, 0x37u << 24, color);
}

void test_rdp_fill8(H64TestContext *ctx)
{
    // ulx, uly, lrx, lry and scissor, in pixels.
    static const u32 cases[][8] = {
        { 0, 0, 303, 269, 0, 0, 304, 270 },     // Banjo-Tooie's whole shadow image
        { 5, 7, 40, 30, 0, 0, 304, 270 },
        { 0, 0, 303, 269, 10, 12, 100, 50 },    // scissor inside
        { 20, 20, 60, 60, 30, 0, 304, 40 },     // scissor across
        { 3, 3, 3, 3, 0, 0, 304, 270 },         // one pixel
    };
    static const u32 colors[] = { 0xFFFEFFFE, 0x12345679, 0x00000000 };
    u32 c, k;
    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
        for (k = 0; k < sizeof(colors) / sizeof(colors[0]); k++)
        {
            const u32 *q = cases[c];
            H64System *a = make_fill_sys(), *b = make_fill_sys();
            u64 w = (u64)((0x36u << 24) | ((q[2] * 4) << 12) | (q[3] * 4)) << 32 | (((q[0] * 4) << 12) | (q[1] * 4));
            u32 lo = IMG - 8, hi = IMG + WIDTH * 272 + 8;
            fill_setup(a, q[4] * 4, q[5] * 4, q[6] * 4, q[7] * 4, colors[k]);
            fill_setup(b, q[4] * 4, q[5] * 4, q[6] * 4, q[7] * 4, colors[k]);
            h64_rdp_command(a, &w, 1);
            H64_CHECK_EQ(ctx, h64_rdp_fill8_fast(b, &w, 1), 1);
            H64_CHECK(ctx, !memcmp(a->rdram + lo, b->rdram + lo, hi - lo));
            H64_CHECK(ctx, !memcmp(a->rdramHidden + lo / 2, b->rdramHidden + lo / 2, (hi - lo) / 2));
            free_fill_sys(a);
            free_fill_sys(b);
        }
    {
        // Anything else is left to the RDP.
        H64System *s = make_fill_sys();
        u64 w = (u64)((0x36u << 24) | (41 << 12) | 40) << 32 | 0;
        fill_setup(s, 0, 0, 304 * 4, 270 * 4, 0xFFFFFFFF);
        H64_CHECK_EQ(ctx, h64_rdp_fill8_fast(s, &w, 1), 0);   // fractional lower right
        free_fill_sys(s);
    }
}
