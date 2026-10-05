// Harissa64 V2 - minimal unit test support.
//
// Tests are plain functions listed in tests/unit/unit_tests.cpp, so the same
// binary runs them on every target (host, big-endian PowerPC under QEMU, and
// the Xbox 360 build at start-up). No static registration: VS2010 and the
// XDK linker drop unreferenced objects from static libraries.
#ifndef H64_TEST_H
#define H64_TEST_H

#include "h64_types.h"

struct H64TestContext
{
    int checks;
    int failures;
    const char *current;
};

typedef void (*H64TestFn)(H64TestContext *ctx);

struct H64TestCase
{
    const char *name;
    H64TestFn fn;
};

void h64_test_fail(H64TestContext *ctx, const char *file, int line, const char *expr);
void h64_test_fail_u64(H64TestContext *ctx, const char *file, int line, const char *expr, u64 got, u64 expected);

#define H64_CHECK(ctx, cond) \
    do { (ctx)->checks++; if (!(cond)) h64_test_fail((ctx), __FILE__, __LINE__, #cond); } while (0)

#define H64_CHECK_EQ(ctx, got, expected) \
    do { u64 h64_g_ = (u64)(got), h64_e_ = (u64)(expected); (ctx)->checks++; \
         if (h64_g_ != h64_e_) h64_test_fail_u64((ctx), __FILE__, __LINE__, #got " == " #expected, h64_g_, h64_e_); } while (0)

// Runs every test; returns the number of failed checks. Results go to the log.
int h64_run_unit_tests(const H64TestCase *tests, int count, int *checksOut);

#endif
