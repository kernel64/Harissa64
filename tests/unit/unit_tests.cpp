// The list of unit tests, shared by every target (h64test on the host and
// under QEMU, and the Xbox 360 build, which runs them at start-up).
#include "unit_tests.h"

void test_types(H64TestContext *ctx);
void test_host_byte_order(H64TestContext *ctx);
void test_bswap(H64TestContext *ctx);
void test_endian_loads(H64TestContext *ctx);
void test_endian_stores(H64TestContext *ctx);
void test_rdp_tmem_rgba16(H64TestContext *ctx);
void test_rdp_tmem_rgba32(H64TestContext *ctx);
void test_rdp_tmem_ia_i(H64TestContext *ctx);
void test_rdp_tmem_tlut(H64TestContext *ctx);
void test_rdp_tmem_block(H64TestContext *ctx);
void test_rdp_tmem_wrap(H64TestContext *ctx);
void test_rdp_tmem_yuv(H64TestContext *ctx);
void test_ppc_emit(H64TestContext *ctx);
void test_fenv(H64TestContext *ctx);
void test_rdp_tri(H64TestContext *ctx);

static const H64TestCase s_tests[] = {
    { "types", test_types },
    { "host_byte_order", test_host_byte_order },
    { "bswap", test_bswap },
    { "endian_loads", test_endian_loads },
    { "endian_stores", test_endian_stores },
    { "rdp_tmem_rgba16", test_rdp_tmem_rgba16 },
    { "rdp_tmem_rgba32", test_rdp_tmem_rgba32 },
    { "rdp_tmem_ia_i", test_rdp_tmem_ia_i },
    { "rdp_tmem_tlut", test_rdp_tmem_tlut },
    { "rdp_tmem_block", test_rdp_tmem_block },
    { "rdp_tmem_wrap", test_rdp_tmem_wrap },
    { "rdp_tmem_yuv", test_rdp_tmem_yuv },
    { "ppc_emit", test_ppc_emit },
    { "fenv", test_fenv },
    { "rdp_tri", test_rdp_tri },
};

int h64_run_all_unit_tests(int *testsOut, int *checksOut)
{
    if (testsOut)
        *testsOut = (int)H64_ARRAY_COUNT(s_tests);
    return h64_run_unit_tests(s_tests, (int)H64_ARRAY_COUNT(s_tests), checksOut);
}
