#include "h64_test.h"
#include "h64_log.h"

void h64_test_fail(H64TestContext *ctx, const char *file, int line, const char *expr)
{
    ctx->failures++;
    H64_ERROR("[test] FAIL %s: %s:%d: %s", ctx->current, file, line, expr);
}

void h64_test_fail_u64(H64TestContext *ctx, const char *file, int line, const char *expr, u64 got, u64 expected)
{
    ctx->failures++;
    H64_ERROR("[test] FAIL %s: %s:%d: %s (got %08X%08X, expected %08X%08X)", ctx->current, file, line, expr,
              (u32)(got >> 32), (u32)got, (u32)(expected >> 32), (u32)expected);
}

int h64_run_unit_tests(const H64TestCase *tests, int count, int *checksOut)
{
    H64TestContext ctx;
    int i, failedTests = 0;
    ctx.checks = 0;
    ctx.failures = 0;
    for (i = 0; i < count; i++)
    {
        int before = ctx.failures;
        ctx.current = tests[i].name;
        tests[i].fn(&ctx);
        if (ctx.failures != before)
            failedTests++;
        H64_INFO("[test] %-28s %s", tests[i].name, ctx.failures != before ? "FAIL" : "ok");
    }
    H64_INFO("[test] %d tests, %d checks, %d failed checks, %d failed tests", count, ctx.checks, ctx.failures,
             failedTests);
    if (checksOut)
        *checksOut = ctx.checks;
    return ctx.failures;
}
