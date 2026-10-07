// Save states (core/savestate/h64_state.cpp): run-length coding and a save /
// load round trip on a small machine.
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_endian.h"
#include "../../core/common/h64_test.h"
#include "../../core/savestate/h64_state.h"
#include "../../core/system/h64_system.h"

static void test_rle(H64TestContext *ctx)
{
    static u8 in[1000], out[1000];
    std::vector<u8> coded;
    int i;
    for (i = 0; i < 1000; i++) in[i] = (u8)(i < 300 ? 0 : i < 310 ? i : i < 700 ? 7 : (i * 37) >> 3);
    h64_rle_encode(in, sizeof(in), &coded);
    H64_CHECK(ctx, coded.size() < 500);
    H64_CHECK_EQ(ctx, h64_rle_decode(&coded[0], (u32)coded.size(), out, sizeof(out)), coded.size());
    H64_CHECK(ctx, !memcmp(in, out, sizeof(in)));
    H64_CHECK_EQ(ctx, h64_rle_decode(&coded[0], (u32)coded.size() - 1, out, sizeof(out)), 0);   // truncated
}

void test_state(H64TestContext *ctx)
{
    // A minimal ROM: 4 KB header + an infinite loop at the entry point.
    static u8 rom[0x101000];
    H64System *sys = (H64System *)malloc(sizeof(H64System));
    std::vector<u8> st, st2;
    u32 ramWord;
    test_rle(ctx);
    memset(rom, 0, sizeof(rom));
    h64_store_be32(rom, 0x80371240u);
    h64_store_be32(rom + 8, 0x80000400u);
    h64_store_be32(rom + 0x1000, 0x1000FFFFu);   // b . (loop)
    if (!sys || h64_system_init(sys, rom, sizeof(rom), 0)) { H64_CHECK(ctx, 0); free(sys); return; }
    h64_system_run_cycles(sys, 100000);
    sys->cpu.gpr[5] = 0x123456789ABCDEF0ull;
    h64_store_be32(sys->rdram + 0x200000, 0xCAFEF00Du);
    sys->save->eeprom[3] = 0x42;
    H64_CHECK(ctx, h64_state_quiet(sys));
    H64_CHECK_EQ(ctx, h64_state_save(sys, &st), 0);
    H64_CHECK(ctx, st.size() > 100 && st.size() < 400000);   // 12 MB of mostly constant memory, coded

    // Change the machine, then load: everything comes back.
    h64_system_run_cycles(sys, 50000);
    sys->cpu.gpr[5] = 0;
    h64_store_be32(sys->rdram + 0x200000, 0);
    sys->save->eeprom[3] = 0;
    H64_CHECK_EQ(ctx, h64_state_load(sys, &st[0], (u32)st.size()), 0);
    ramWord = h64_load_be32(sys->rdram + 0x200000);
    H64_CHECK_EQ(ctx, sys->cpu.gpr[5], 0x123456789ABCDEF0ull);
    H64_CHECK_EQ(ctx, ramWord, 0xCAFEF00Du);
    H64_CHECK_EQ(ctx, sys->save->eeprom[3], 0x42);
    // Saving again gives the same bytes.
    h64_state_save(sys, &st2);
    H64_CHECK(ctx, st == st2);

    // Bad states are rejected and leave the machine alone.
    sys->cpu.gpr[5] = 7;
    H64_CHECK_EQ(ctx, h64_state_load(sys, &st[0], (u32)st.size() - 3), -1);
    st[4] ^= 1;   // format version
    H64_CHECK_EQ(ctx, h64_state_load(sys, &st[0], (u32)st.size()), -1);
    H64_CHECK_EQ(ctx, sys->cpu.gpr[5], 7);
    h64_system_free(sys);
    free(sys);
}
