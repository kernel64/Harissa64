// Unit test for render test scene playback (core/rtest/h64_rtscene.h): a
// scene built in memory (little-endian words, RDRAM in PC word order) played
// with the software RDP and with a renderer, on every target's byte order.
#include "unit_tests.h"

#include <stdlib.h>
#include <string.h>

#include <vector>

#include "../../core/common/h64_endian.h"
#include "../../core/rdp/h64_rdp.h"
#include "../../core/rtest/h64_rtscene.h"
#include "../../core/system/h64_system.h"
#include "../../render/api.h"

static void put(std::vector<u8> &v, u32 w)
{
    v.push_back((u8)w); v.push_back((u8)(w >> 8)); v.push_back((u8)(w >> 16)); v.push_back((u8)(w >> 24));
}

static void put_cmd(std::vector<u8> &v, u32 hi, u32 lo)
{
    put(v, 2); put(v, (hi >> 24) & 0x3F); put(v, 2); put(v, hi); put(v, lo);
}

struct RtCounts { u32 tris, cmds; float x0; };

static void cnt_rdp(void *user, const u64 *, u32) { ((RtCounts *)user)->cmds++; }
static void cnt_tri(void *user, const H64RenderVertex *a, const H64RenderVertex *, const H64RenderVertex *, u32, u32, u32)
{
    RtCounts *c = (RtCounts *)user;
    c->tris++;
    c->x0 = a->x;
}

void test_rtscene(H64TestContext *ctx)
{
    std::vector<u8> v;
    H64RtScene sc;
    H64System *sys = (H64System *)calloc(1, sizeof(H64System));
    u32 k;
    float fx = 1.5f;
    u32 fxw;
    sys->rdram = (u8 *)calloc(1, H64_RDRAM_SIZE);
    v.insert(v.end(), (const u8 *)"RDPDUMP2", (const u8 *)"RDPDUMP2" + 8);
    put(v, H64_RDRAM_SIZE); put(v, H64_RDRAM_SIZE / 2);
    // N64 bytes 01..08 at 0x1000, as the PC's words: 04 03 02 01 08 07 06 05.
    put(v, 1); put(v, 0x1000); put(v, 8);
    put(v, 0x01020304); put(v, 0x05060708);
    put(v, 7);
    put_cmd(v, 0x3F100007, 0x2000);           // SET_COLOR_IMAGE RGBA 16-bit, width 8, at 0x2000
    put_cmd(v, 0x2D000000, (8 << 14) | (8 << 2));   // SET_SCISSOR 0,0 - 8,8
    put_cmd(v, 0x2F300000, 0);                // SET_OTHER_MODES: fill
    put_cmd(v, 0x37000000, 0xF801F801);       // SET_FILL_COLOR: red
    put_cmd(v, 0x36000000 | (7 << 14) | (7 << 2), 0);   // FILL_RECTANGLE 0,0 - 7,7
    put(v, 3); put(v, 1); put(v, 0x2000);     // VI_ORIGIN
    put(v, 4);
    // Frame 2: an HLE triangle record then its RDP command (not drawn here).
    memcpy(&fxw, &fx, 4);
    put(v, H64_RT_REC_HLE_TRIANGLE); put(v, 7); put(v, 1); put(v, 1);
    for (k = 0; k < 30; k++) put(v, k == 0 ? fxw : 0);
    put_cmd(v, 0x08000000, 0);
    put_cmd(v, 0x27000000, 0);                // SYNC_PIPE: an ordinary command
    put(v, 4);
    put(v, 6);

    H64_CHECK(ctx, h64_rtscene_open(&sc, &v[0], (u32)v.size()) == 0);
    H64_CHECK_EQ(ctx, h64_rtscene_play_frame(&sc, sys, 0), 1);
    for (k = 0; k < 8; k++) H64_CHECK_EQ(ctx, sys->rdram[0x1000 + k], k + 1);
    H64_CHECK_EQ(ctx, h64_load_be16(sys->rdram + 0x2000), 0xF801u);
    H64_CHECK_EQ(ctx, h64_load_be16(sys->rdram + 0x2000 + 7 * 16 + 14), 0xF801u);
    H64_CHECK_EQ(ctx, h64_load_be16(sys->rdram + 0x2000 + 8 * 16), 0u);   // below the rectangle
    H64_CHECK_EQ(ctx, sys->vi.regs[1], 0x2000u);
    {
        RtCounts c;
        H64Renderer r;
        memset(&c, 0, sizeof(c));
        memset(&r, 0, sizeof(r));
        r.user = &c;
        r.rdp = cnt_rdp;
        r.triangle = cnt_tri;
        H64_CHECK_EQ(ctx, h64_rtscene_play_frame(&sc, sys, &r), 1);
        H64_CHECK_EQ(ctx, c.tris, 1u);
        H64_CHECK_EQ(ctx, c.cmds, 1u);   // the triangle's RDP command is not sent, SYNC_PIPE is
        H64_CHECK(ctx, c.x0 == 1.5f);
    }
    H64_CHECK_EQ(ctx, h64_rtscene_play_frame(&sc, sys, 0), 0);
    H64_CHECK_EQ(ctx, sc.frame, 2u);
    h64_rdp_free(sys);
    free(sys->rdram);
    free(sys);
}
