#include "h64_fenv.h"

#if defined(_MSC_VER)
#include <float.h>

void h64_fenv_init(void)
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

u32 h64_fenv_flags(void)
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

void h64_fenv_init(void)
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

u32 h64_fenv_flags(void)
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
