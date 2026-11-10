// Cheats (core/cheat/h64_cheat.cpp): reading the database format and a
// user's file, and what each kind of code does to a small memory.
#include <string.h>

#include "../../core/cheat/h64_cheat.h"
#include "../../core/common/h64_test.h"

static u8 s_mem[0x100];

static u32 mem_read(void *user, u32 addr, int bytes)
{
    (void)user;
    addr &= 0xFF;
    return bytes == 1 ? s_mem[addr] : ((u32)s_mem[addr] << 8) | s_mem[(addr + 1) & 0xFF];
}

static void mem_write(void *user, u32 addr, int bytes, u32 value)
{
    (void)user;
    addr &= 0xFF;
    if (bytes == 1) s_mem[addr] = (u8)value;
    else { s_mem[addr] = (u8)(value >> 8); s_mem[(addr + 1) & 0xFF] = (u8)value; }
}

static const char s_db[] =
    "// a comment\n"
    "crc 11111111-22222222-C:45\n"
    "gn OTHER GAME\n"
    " cn Not this one\n"
    "  80000010 00FF\n"
    "crc 635A2BFF-8B022326-C:45\n"
    "gn TEST GAME\n"
    " cn Infinite\\Lives\n"
    "  cd A note.\n"
    "  80000010 0064\n"
    " cn Sixteen bits\n"
    "  81000020 1234\n"
    " cn Laps\n"
    "  80000030 ???? 0001:\"1 Lap\",0003:\"3 Laps\"\n"
    " cn If equal\n"
    "  D0000040 0005\n"
    "  80000041 0077\n"
    " cn If different\n"
    "  D2000040 0005\n"
    "  80000042 0088\n"
    " cn Repeat\n"
    "  50000302 0010\n"
    "  80000050 0001\n"
    " cn Once\n"
    "  F0000060 0009\n"
    " cn Button\n"
    "  88000070 00AA\n"
    "crc 33333333-44444444-C:4A\n"
    " cn After\n"
    "  80000011 0001\n";

void test_cheat(H64TestContext *ctx)
{
    H64CheatList *l = h64_cheat_new();
    H64CheatMem mem;
    H64CheatInfo info;
    char states[128];
    mem.user = NULL;
    mem.read = mem_read;
    mem.write = mem_write;
    memset(s_mem, 0, sizeof(s_mem));

    H64_CHECK_EQ(ctx, h64_cheat_load_db(l, s_db, sizeof(s_db) - 1, 0x11112222, 0x33334444, 0x45), 0);
    H64_CHECK_EQ(ctx, h64_cheat_load_db(l, s_db, sizeof(s_db) - 1, 0x635A2BFF, 0x8B022326, 0x45), 8);
    H64_CHECK_EQ(ctx, h64_cheat_count(l), 8);
    h64_cheat_info(l, 0, &info);
    H64_CHECK(ctx, !strcmp(info.name, "Infinite\\Lives"));
    H64_CHECK(ctx, !strcmp(info.note, "A note."));
    H64_CHECK_EQ(ctx, info.options, 0);
    h64_cheat_info(l, 2, &info);
    H64_CHECK_EQ(ctx, info.options, 2);
    H64_CHECK(ctx, !strcmp(h64_cheat_option_name(l, 2, 1), "3 Laps"));

    // Nothing on: nothing written.
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x10], 0);

    // Plain writes, 8 and 16 bits, kept every frame, and put back when turned off.
    s_mem[0x10] = 3;
    h64_cheat_set(l, 0, 1);
    h64_cheat_set(l, 1, 1);
    H64_CHECK_EQ(ctx, h64_cheat_active(l), 2);
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x10], 0x64);
    H64_CHECK_EQ(ctx, s_mem[0x20], 0x12);
    H64_CHECK_EQ(ctx, s_mem[0x21], 0x34);
    s_mem[0x10] = 1;
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x10], 0x64);
    h64_cheat_set(l, 0, 0);
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x10], 3);
    H64_CHECK_EQ(ctx, s_mem[0x20], 0x12);   // the other one stays on

    // A choice: the value of the option picked.
    h64_cheat_set(l, 2, 2);
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x30], 3);
    h64_cheat_set(l, 2, 1);
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x30], 1);
    h64_cheat_set(l, 2, 9);   // out of range: off
    h64_cheat_info(l, 2, &info);
    H64_CHECK_EQ(ctx, info.state, 0);

    // Conditions.
    h64_cheat_set(l, 3, 1);
    h64_cheat_set(l, 4, 1);
    s_mem[0x40] = 4;
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x41], 0);
    H64_CHECK_EQ(ctx, s_mem[0x42], 0x88);
    s_mem[0x40] = 5;
    s_mem[0x42] = 0;
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x41], 0x77);
    H64_CHECK_EQ(ctx, s_mem[0x42], 0);

    // Repeat: 3 times, address + 2, value + 0x10.
    h64_cheat_set(l, 5, 1);
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x50], 0x01);
    H64_CHECK_EQ(ctx, s_mem[0x52], 0x11);
    H64_CHECK_EQ(ctx, s_mem[0x54], 0x21);
    H64_CHECK_EQ(ctx, s_mem[0x56], 0);

    // Once, when turned on; the button code only with the button.
    h64_cheat_set(l, 6, 1);
    h64_cheat_set(l, 7, 1);
    h64_cheat_run(l, &mem, 0);
    H64_CHECK_EQ(ctx, s_mem[0x60], 9);
    H64_CHECK_EQ(ctx, s_mem[0x70], 0);
    s_mem[0x60] = 0;
    h64_cheat_run(l, &mem, 1);
    H64_CHECK_EQ(ctx, s_mem[0x60], 0);
    H64_CHECK_EQ(ctx, s_mem[0x70], 0xAA);

    // The states go to text and back.
    h64_cheat_states_write(l, states, sizeof(states));
    {
        H64CheatList *m = h64_cheat_new();
        h64_cheat_load_db(m, s_db, sizeof(s_db) - 1, 0x635A2BFF, 0x8B022326, 0x45);
        h64_cheat_states_read(m, states);
        H64_CHECK_EQ(ctx, h64_cheat_active(m), h64_cheat_active(l));
        h64_cheat_info(m, 1, &info);
        H64_CHECK_EQ(ctx, info.state, 1);
        h64_cheat_info(m, 0, &info);
        H64_CHECK_EQ(ctx, info.state, 0);
        h64_cheat_states_read(m, "garbage,12345678:1,zz");
        h64_cheat_free(m);
    }

    // A user's file: names without "cn", comments, codes.
    {
        static const char plain[] = "# mine\nMy cheat\n80000080 0042\ncn Second\n 81000082 BEEF\n";
        H64CheatList *u = h64_cheat_new();
        H64_CHECK_EQ(ctx, h64_cheat_load_plain(u, plain, sizeof(plain) - 1), 2);
        h64_cheat_info(u, 0, &info);
        H64_CHECK(ctx, !strcmp(info.name, "My cheat"));
        h64_cheat_set(u, 0, 1);
        h64_cheat_set(u, 1, 1);
        h64_cheat_run(u, &mem, 0);
        H64_CHECK_EQ(ctx, s_mem[0x80], 0x42);
        H64_CHECK_EQ(ctx, s_mem[0x82], 0xBE);
        H64_CHECK_EQ(ctx, s_mem[0x83], 0xEF);
        h64_cheat_free(u);
    }
    h64_cheat_free(l);
}
