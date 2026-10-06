// Host floating-point exception flags, as the reference FPU reads them
// (core/common/h64_fenv). Run on every target, the Xbox included, where the
// XDK reads them through _clearfp().
#include "unit_tests.h"
#include "../../core/common/h64_fenv.h"

// volatile: keep the operations at run time.
static volatile float s_one = 1.0f, s_three = 3.0f, s_big = 1e30f, s_zero = 0.0f;
static volatile s32 s_int = 0x01000001;   // not exactly representable in a float

void test_fenv(H64TestContext *ctx)
{
    volatile float r;
    h64_fenv_init();
    if (!h64_fenv_reliable())
    {
        // Xenia: the flags are emulated as all set; h64_fenv reports none instead.
        H64_CHECK_EQ(ctx, h64_fenv_flags(), 0u);
        return;
    }

    h64_fenv_clear();
    r = s_one + s_one;
    H64_CHECK_EQ(ctx, h64_fenv_flags(), 0u);

    h64_fenv_clear();
    r = s_one / s_three;
    H64_CHECK_EQ(ctx, h64_fenv_flags(), H64_FE_INEXACT);

    h64_fenv_clear();
    r = (float)s_int;
    H64_CHECK_EQ(ctx, h64_fenv_flags(), H64_FE_INEXACT);

    h64_fenv_clear();
    r = (float)(s32)1000;
    H64_CHECK_EQ(ctx, h64_fenv_flags(), 0u);

    h64_fenv_clear();
    r = s_big * s_big;
    H64_CHECK_EQ(ctx, h64_fenv_flags() & (H64_FE_OVERFLOW | H64_FE_INVALID), H64_FE_OVERFLOW);

    h64_fenv_clear();
    r = s_zero / s_zero;
    H64_CHECK_EQ(ctx, h64_fenv_flags() & H64_FE_INVALID, H64_FE_INVALID);

    h64_fenv_clear();
    r = s_one / s_zero;
    H64_CHECK_EQ(ctx, h64_fenv_flags() & (H64_FE_DIVZERO | H64_FE_INVALID), H64_FE_DIVZERO);
    (void)r;
}
