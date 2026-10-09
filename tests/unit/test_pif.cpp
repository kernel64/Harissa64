// JoyBus controller ports (core/pif/h64_pif.cpp): the plugged ports answer the
// info and read commands with their own controller, the others "no response".
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_endian.h"
#include "../../core/common/h64_test.h"
#include "../../core/system/h64_system.h"

// One command per channel 0-3 (tx 1 byte, rx `rxLen`), as osContStartQuery /
// osContStartReadData lay them out; returns each channel's rx-length byte offset.
static void pif_block(H64System *sys, u8 cmd, u8 rxLen, int at[4])
{
    int ch, i = 0;
    memset(sys->pifRam, 0, sizeof(sys->pifRam));
    for (ch = 0; ch < 4; ch++)
    {
        sys->pifRam[i++] = 0x01;
        at[ch] = i;
        sys->pifRam[i++] = rxLen;
        sys->pifRam[i++] = cmd;
        i += rxLen;
    }
    sys->pifRam[i] = 0xFE;
    sys->pifRam[0x3F] = 0x01;
    h64_pif_run_commands(sys);
}

void test_pif(H64TestContext *ctx)
{
    static u8 rom[0x101000];
    H64System *sys = (H64System *)malloc(sizeof(H64System));
    int at[4], ch;
    memset(rom, 0, sizeof(rom));
    h64_store_be32(rom, 0x80371240u);
    h64_store_be32(rom + 8, 0x80000400u);
    h64_store_be32(rom + 0x1000, 0x1000FFFFu);   // b . (loop)
    if (!sys || h64_system_init(sys, rom, sizeof(rom), 0)) { H64_CHECK(ctx, 0); free(sys); return; }

    // Default: port 1 only.
    pif_block(sys, 0x00, 3, at);
    H64_CHECK_EQ(ctx, sys->pifRam[at[0]], 0x03);
    H64_CHECK_EQ(ctx, sys->pifRam[at[0] + 2], 0x05);   // standard controller
    for (ch = 1; ch < 4; ch++) H64_CHECK_EQ(ctx, sys->pifRam[at[ch]], 0x83);   // no response

    // Ports 1, 2 and 4: each read returns its own controller.
    sys->padMask = 0x0B;
    for (ch = 0; ch < 4; ch++)
    {
        sys->pad[ch].buttons = (u16)(0x1000 << ch);
        sys->pad[ch].x = (s8)(10 + ch);
        sys->pad[ch].y = (s8)(-20 - ch);
    }
    pif_block(sys, 0x00, 3, at);
    H64_CHECK_EQ(ctx, sys->pifRam[at[1]], 0x03);
    H64_CHECK_EQ(ctx, sys->pifRam[at[1] + 2], 0x05);
    H64_CHECK_EQ(ctx, sys->pifRam[at[2]], 0x83);
    H64_CHECK_EQ(ctx, sys->pifRam[at[3]], 0x03);
    pif_block(sys, 0x01, 4, at);
    for (ch = 0; ch < 4; ch++)
    {
        if (ch == 2) { H64_CHECK_EQ(ctx, sys->pifRam[at[ch]], 0x84); continue; }
        H64_CHECK_EQ(ctx, sys->pifRam[at[ch]], 0x04);
        H64_CHECK_EQ(ctx, h64_load_be16(sys->pifRam + at[ch] + 2), (u32)(0x1000 << ch));
        H64_CHECK_EQ(ctx, (s8)sys->pifRam[at[ch] + 4], 10 + ch);
        H64_CHECK_EQ(ctx, (s8)sys->pifRam[at[ch] + 5], -20 - ch);
    }
    h64_system_free(sys);
    free(sys);
}
