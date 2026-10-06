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

#if defined(_XBOX)
// The XDK CRT's _clearfp does not return the FPSCR flags (it returned 0x1F
// for every operation on the console): read and write FPSCR directly. No
// intrinsic exists, so the instructions are emitted in non-inlined functions
// whose pointer argument arrives in r3 (as FlushLine in tools/execmem).
#include <ppcintrinsics.h>

__declspec(noinline) static void fpscr_read(double *out)
{
    __emit(0xFC00048E);   // mffs  f0
    __emit(0xD8030000);   // stfd  f0, 0(r3)
}

__declspec(noinline) static void fpscr_write(const double *in)
{
    __emit(0xC8030000);   // lfd   f0, 0(r3)
    __emit(0xFDFE058E);   // mtfsf 0xFF, f0
}

// Called through volatile function pointers: with whole-program optimisation
// (LTCG) a direct call to a static function may not pass the pointer in r3
// (the first version crashed the console at start-up). An indirect call
// always uses the standard convention.
static void (*volatile s_fpscrRead)(double *) = fpscr_read;
static void (*volatile s_fpscrWrite)(const double *) = fpscr_write;

static u32 fpscr_get(void)
{
    union { double d; u64 u; } v;
    v.u = 0;
    s_fpscrRead(&v.d);
    return (u32)v.u;
}

static void clear_flags(void)
{
    union { double d; u64 u; } v;
    v.u = 0;
    s_fpscrRead(&v.d);
    v.u &= ~(u64)0xFFFFFF00u;   // keep the enables, NI and the rounding mode; clear every status bit
    s_fpscrWrite(&v.d);
}

static u32 read_flags(void)
{
    u32 s = fpscr_get(), f = 0;
    if (s & 0x02000000u) f |= H64_FE_INEXACT;     // XX
    if (s & 0x08000000u) f |= H64_FE_UNDERFLOW;   // UX
    if (s & 0x10000000u) f |= H64_FE_OVERFLOW;    // OX
    if (s & 0x04000000u) f |= H64_FE_DIVZERO;     // ZX
    if (s & 0x20000000u) f |= H64_FE_INVALID;     // VX
    return f;
}
#else
static void clear_flags(void) { _clearfp(); }

static u32 read_flags(void)
{
    unsigned int s = _statusfp();
    u32 f = 0;
    if (s & _SW_INEXACT) f |= H64_FE_INEXACT;
    if (s & _SW_UNDERFLOW) f |= H64_FE_UNDERFLOW;
    if (s & _SW_OVERFLOW) f |= H64_FE_OVERFLOW;
    if (s & _SW_ZERODIVIDE) f |= H64_FE_DIVZERO;
    if (s & _SW_INVALID) f |= H64_FE_INVALID;
    return f;
}
#endif

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

static void clear_flags(void) { feclearexcept(FE_ALL_EXCEPT); }

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

// Self-test of the host flags: when they do not behave, the FPU reports
// none. Xenia must not even read them: it does not implement mffs/mtfsf and
// stops (the Xbox front end calls h64_fenv_disable_host_flags with xenia=1).
#include "h64_log.h"

static int s_checked, s_reliable = 1;

void h64_fenv_disable_host_flags(void)
{
    s_checked = 1;
    s_reliable = 0;
}

void h64_fenv_clear(void)
{
    if (s_reliable) clear_flags();
}

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
