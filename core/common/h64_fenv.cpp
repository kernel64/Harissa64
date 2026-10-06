#include "h64_fenv.h"

#if defined(_MSC_VER)
#include <float.h>

static void init_env(void)
{
#if defined(_DN_SAVE) && defined(_MCW_DN)
    _controlfp(_DN_SAVE, _MCW_DN);
#endif
    _controlfp(_MCW_EM, _MCW_EM);   // mask every exception: we read the flags instead
    _controlfp(_RC_NEAR, _MCW_RC);
}

void h64_fenv_set_round(int rm)
{
    static const unsigned int modes[4] = { _RC_NEAR, _RC_CHOP, _RC_UP, _RC_DOWN };
    _controlfp(modes[rm & 3], _MCW_RC);
}

void h64_fenv_clear(void) { _clearfp(); }

static u32 read_flags(void)
{
#if defined(_XBOX)
    // The XDK CRT declares _statusfp but does not provide it; _clearfp returns
    // the status word before clearing it, which is fine for a single read.
    unsigned int s = _clearfp();
#else
    unsigned int s = _statusfp();
#endif
    u32 f = 0;
    if (s & _SW_INEXACT) f |= H64_FE_INEXACT;
    if (s & _SW_UNDERFLOW) f |= H64_FE_UNDERFLOW;
    if (s & _SW_OVERFLOW) f |= H64_FE_OVERFLOW;
    if (s & _SW_ZERODIVIDE) f |= H64_FE_DIVZERO;
    if (s & _SW_INVALID) f |= H64_FE_INVALID;
    return f;
}

#else
#include <fenv.h>

static void init_env(void)
{
    fesetround(FE_TONEAREST);
    feclearexcept(FE_ALL_EXCEPT);
}

void h64_fenv_set_round(int rm)
{
    static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    fesetround(modes[rm & 3]);
}

void h64_fenv_clear(void) { feclearexcept(FE_ALL_EXCEPT); }

static u32 read_flags(void)
{
    int s = fetestexcept(FE_ALL_EXCEPT);
    u32 f = 0;
    if (s & FE_INEXACT) f |= H64_FE_INEXACT;
    if (s & FE_UNDERFLOW) f |= H64_FE_UNDERFLOW;
    if (s & FE_OVERFLOW) f |= H64_FE_OVERFLOW;
    if (s & FE_DIVBYZERO) f |= H64_FE_DIVZERO;
    if (s & FE_INVALID) f |= H64_FE_INVALID;
    return f;
}
#endif

// Self-test: Xenia does not emulate the PowerPC FPSCR exception flags (they
// read back all set), which would make every guest FPU operation raise an
// exception. When the host flags do not behave, report none instead.
#include "h64_log.h"

static int s_checked, s_reliable = 1;

void h64_fenv_init(void)
{
    init_env();
    if (!s_checked)
    {
        volatile float one = 1.0f, three = 3.0f, r;
        u32 a, b;
        h64_fenv_clear();
        r = one + one;
        a = read_flags();
        h64_fenv_clear();
        r = one / three;
        b = read_flags();
        h64_fenv_clear();
        (void)r;
        s_reliable = a == 0 && b == H64_FE_INEXACT;
        s_checked = 1;
        if (!s_reliable)
            H64_WARN("[fpu] host exception flags are not reliable (read %02X and %02X): the FPU reports none", a, b);
    }
}

int h64_fenv_reliable(void) { return s_reliable; }

u32 h64_fenv_flags(void) { return s_reliable ? read_flags() : 0; }
