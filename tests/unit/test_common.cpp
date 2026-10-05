// Unit tests for core/common: type sizes and endian helpers.
#include "../../core/common/h64_types.h"
#include "../../core/common/h64_endian.h"
#include "../../core/common/h64_test.h"

void test_types(H64TestContext *ctx)
{
    H64_CHECK_EQ(ctx, sizeof(u8), 1);
    H64_CHECK_EQ(ctx, sizeof(u16), 2);
    H64_CHECK_EQ(ctx, sizeof(u32), 4);
    H64_CHECK_EQ(ctx, sizeof(u64), 8);
    H64_CHECK_EQ(ctx, sizeof(s64), 8);
    {
        u8 byte = 0x80;
        H64_CHECK_EQ(ctx, (s32)(s8)byte, -128);
    }
    H64_CHECK_EQ(ctx, (u64)(s64)(s32)0x80000000u, 0xFFFFFFFF80000000ull);
    // Arithmetic shift of negative values is relied on by the CPU core.
    H64_CHECK_EQ(ctx, (u64)((s64)-16 >> 2), (u64)(s64)-4);
    H64_CHECK_EQ(ctx, (u32)((s32)0x80000000 >> 31), 0xFFFFFFFFu);
}

void test_host_byte_order(H64TestContext *ctx)
{
    u32 v = 0x11223344u;
    u8 b[4];
    memcpy(b, &v, 4);
#if H64_HOST_BIG_ENDIAN
    H64_CHECK_EQ(ctx, b[0], 0x11);
#else
    H64_CHECK_EQ(ctx, b[0], 0x44);
#endif
}

void test_bswap(H64TestContext *ctx)
{
    H64_CHECK_EQ(ctx, h64_bswap16(0x1234), 0x3412);
    H64_CHECK_EQ(ctx, h64_bswap32(0x11223344u), 0x44332211u);
    H64_CHECK_EQ(ctx, h64_bswap64(0x0102030405060708ull), 0x0807060504030201ull);
}

void test_endian_loads(H64TestContext *ctx)
{
    // Guest memory is N64 byte order: the first byte is the most significant.
    static const u8 mem[16] = { 0x80, 0x37, 0x12, 0x40, 0x00, 0x00, 0x00, 0x0F,
                                0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67 };
    int off;
    H64_CHECK_EQ(ctx, h64_load_be32(mem), 0x80371240u);     // a z64 ROM header
    H64_CHECK_EQ(ctx, h64_load_be16(mem + 2), 0x1240);
    H64_CHECK_EQ(ctx, h64_load_be64(mem + 8), 0xDEADBEEF01234567ull);
    // Fast and reference paths agree at every (also unaligned) offset.
    for (off = 0; off <= 8; off++)
    {
        H64_CHECK_EQ(ctx, h64_load_be16(mem + off), h64_ref_load_be16(mem + off));
        H64_CHECK_EQ(ctx, h64_load_be32(mem + off), h64_ref_load_be32(mem + off));
        H64_CHECK_EQ(ctx, h64_load_be64(mem + off), h64_ref_load_be64(mem + off));
    }
}

void test_endian_stores(H64TestContext *ctx)
{
    u8 a[16], b[16];
    int off;
    for (off = 0; off <= 8; off++)
    {
        memset(a, 0xAA, sizeof(a));
        memset(b, 0xAA, sizeof(b));
        h64_store_be64(a + off, 0x0102030405060708ull);
        h64_ref_store_be64(b + off, 0x0102030405060708ull);
        H64_CHECK(ctx, memcmp(a, b, sizeof(a)) == 0);
        H64_CHECK_EQ(ctx, a[off], 0x01);
        H64_CHECK_EQ(ctx, a[off + 7], 0x08);

        h64_store_be32(a + off, 0xCAFEF00Du);
        h64_ref_store_be32(b + off, 0xCAFEF00Du);
        H64_CHECK(ctx, memcmp(a, b, sizeof(a)) == 0);

        h64_store_be16(a + off, 0xBEEF);
        h64_ref_store_be16(b + off, 0xBEEF);
        H64_CHECK(ctx, memcmp(a, b, sizeof(a)) == 0);
        H64_CHECK_EQ(ctx, h64_load_be16(a + off), 0xBEEF);
    }
}
