// Harissa64 V2 - VR4300 COP1 (FPU), reference implementation.
//
// Arithmetic runs on the host's IEEE unit in the guest's rounding mode; the
// host exception flags become the FCR31 cause bits. What the host cannot
// express is handled explicitly, following the VR4300 manual (chapter 7,
// "Floating-point exceptions"):
//  - Unimplemented Operation (cause E): denormal operands, denormal results
//    with FS=0, conversions of NaN/infinity/out-of-range values to integers,
//    and integer-to-float conversions of values beyond 2^55. E always traps.
//  - NaN results are the MIPS default NaN (single 0x7FBFFFFF, double
//    0x7FF7FFFFFFFFFFFF); a signalling NaN operand raises Invalid.
// A trapping operation does not write its destination.
#include "h64_cpu_internal.h"

#include <math.h>
#include <string.h>

#include "../common/h64_fenv.h"

#if defined(_MSC_VER)
#pragma fenv_access(on)
#endif

#define FCR0_VALUE 0x00000A00u
#define NAN_S 0x7FBFFFFFu
#define NAN_D 0x7FF7FFFFFFFFFFFFull

#define CAUSE_I 0x01u
#define CAUSE_U 0x02u
#define CAUSE_O 0x04u
#define CAUSE_Z 0x08u
#define CAUSE_V 0x10u
#define CAUSE_E 0x20u

// ---- Register file (FR=0: 16 even/odd pairs; FR=1: 32 x 64-bit) ----
static int fr(H64Cpu *cpu) { return (cpu->cop0[CP0_STATUS] & SR_FR) != 0; }

u32 h64_fpr_get32(H64Cpu *cpu, int n)
{
    if (fr(cpu)) return (u32)cpu->fgr[n];
    return (n & 1) ? (u32)(cpu->fgr[n & ~1] >> 32) : (u32)cpu->fgr[n];
}

void h64_fpr_set32(H64Cpu *cpu, int n, u32 v)
{
    if (fr(cpu)) cpu->fgr[n] = (cpu->fgr[n] & 0xFFFFFFFF00000000ull) | v;
    else if (n & 1) cpu->fgr[n & ~1] = (cpu->fgr[n & ~1] & 0xFFFFFFFFull) | ((u64)v << 32);
    else cpu->fgr[n] = (cpu->fgr[n] & 0xFFFFFFFF00000000ull) | v;
}

u64 h64_fpr_get64(H64Cpu *cpu, int n) { return cpu->fgr[fr(cpu) ? n : (n & ~1)]; }

// Computational instructions (n64-systemtest full_vs_half_mode): a 32-bit
// result is written zero-extended to the whole 64-bit register fd (also in
// half mode, where it clobbers the odd half); a single-precision fs operand
// with an odd index reads the even register in half mode, ft is read as is.
static void set_result32(H64Cpu *cpu, int fd, u32 v) { cpu->fgr[fd] = v; }
static u32 get_fs32(H64Cpu *cpu, int fs) { return (u32)cpu->fgr[fr(cpu) ? fs : (fs & ~1)]; }
static u32 get_ft32(H64Cpu *cpu, int ft) { return (u32)cpu->fgr[ft]; }
void h64_fpr_set64(H64Cpu *cpu, int n, u64 v) { cpu->fgr[fr(cpu) ? n : (n & ~1)] = v; }
// The same rules for 64-bit operands and results: fs drops its low bit in
// half mode, ft and fd are used as is.
static void set_result64(H64Cpu *cpu, int fd, u64 v) { cpu->fgr[fd] = v; }
static u64 get_fs64(H64Cpu *cpu, int fs) { return cpu->fgr[fr(cpu) ? fs : (fs & ~1)]; }

// ---- Bit-level classification ----
static float f_from(u32 b) { float f; memcpy(&f, &b, 4); return f; }
static u32 f_bits(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static double d_from(u64 b) { double d; memcpy(&d, &b, 8); return d; }
static u64 d_bits(double d) { u64 b; memcpy(&b, &d, 8); return b; }

static int s_is_nan(u32 b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu); }
static int s_is_snan(u32 b) { return s_is_nan(b) && (b & 0x400000u); }   // MIPS: quiet bit 0 = quiet? see below
static int s_is_denormal(u32 b) { return (b & 0x7F800000u) == 0 && (b & 0x7FFFFFu); }
static int s_is_inf(u32 b) { return (b & 0x7FFFFFFFu) == 0x7F800000u; }
static int d_is_nan(u64 b) { return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0xFFFFFFFFFFFFFull); }
static int d_is_snan(u64 b) { return d_is_nan(b) && (b & 0x8000000000000ull); }
static int d_is_denormal(u64 b) { return (b & 0x7FF0000000000000ull) == 0 && (b & 0xFFFFFFFFFFFFFull); }
static int d_is_inf(u64 b) { return (b & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
// Legacy MIPS NaN encoding: a NaN whose top mantissa bit is SET is signalling
// (the reverse of IEEE 754-2008), which is why the default NaN is 0x7FBFFFFF.

// ---- Exception bookkeeping ----
// Sets the cause bits; returns 1 if the operation must trap (and not write).
static int fpu_finish(H64Cpu *cpu, u32 cause)
{
    u32 enables = (cpu->fcr31 >> 7) & 0x1F;
    cpu->fcr31 = (cpu->fcr31 & ~FCR31_CAUSE) | (cause << 12);
    if ((cause & CAUSE_E) || (cause & enables))
    {
        h64_cpu_exception(cpu, EXC_FPE, 0x180);
        return 1;
    }
    cpu->fcr31 |= (cause & 0x1F) << 2;   // sticky flags
    return 0;
}

static void fpu_begin(H64Cpu *cpu)
{
    h64_fenv_begin((int)(cpu->fcr31 & 3));
}

static u32 host_cause(void)
{
    return h64_fenv_flags();   // same bit order as CAUSE_I..CAUSE_V
}

// Results. A tiny result (host underflow flag, or a denormal) is an
// Unimplemented Operation, except with FS=1 and the Underflow and Inexact
// exceptions both disabled: then it is flushed and Underflow + Inexact are
// signalled. The flushed value depends on the rounding mode: +/-0, or the
// smallest normal number when rounding away from zero towards that sign
// (n64-systemtest COP1 arithmetic tests).
static int flush_allowed(H64Cpu *cpu)
{
    u32 enables = (cpu->fcr31 >> 7) & 0x1F;
    return (cpu->fcr31 & FCR31_FS) && !(enables & (CAUSE_U | CAUSE_I));
}

static int s_result(H64Cpu *cpu, float r, u32 cause, u32 *out)
{
    u32 b = f_bits(r);
    if (s_is_nan(b)) b = NAN_S;
    else if ((cause & CAUSE_U) || s_is_denormal(b))
    {
        int neg = (b >> 31) != 0, rm = (int)(cpu->fcr31 & 3);
        if (!flush_allowed(cpu)) { fpu_finish(cpu, CAUSE_E); return -1; }
        if ((rm == H64_RM_UP && !neg) || (rm == H64_RM_DOWN && neg)) b = (neg ? 0x80000000u : 0) | 0x00800000u;
        else b = neg ? 0x80000000u : 0;
        cause = (cause & (CAUSE_Z | CAUSE_V | CAUSE_O)) | CAUSE_U | CAUSE_I;
    }
    if (fpu_finish(cpu, cause)) return -1;
    *out = b;
    return 0;
}

static int d_result(H64Cpu *cpu, double r, u32 cause, u64 *out)
{
    u64 b = d_bits(r);
    if (d_is_nan(b)) b = NAN_D;
    else if ((cause & CAUSE_U) || d_is_denormal(b))
    {
        int neg = (b >> 63) != 0, rm = (int)(cpu->fcr31 & 3);
        if (!flush_allowed(cpu)) { fpu_finish(cpu, CAUSE_E); return -1; }
        if ((rm == H64_RM_UP && !neg) || (rm == H64_RM_DOWN && neg))
            b = (neg ? 0x8000000000000000ull : 0) | 0x0010000000000000ull;
        else b = neg ? 0x8000000000000000ull : 0;
        cause = (cause & (CAUSE_Z | CAUSE_V | CAUSE_O)) | CAUSE_U | CAUSE_I;
    }
    if (fpu_finish(cpu, cause)) return -1;
    *out = b;
    return 0;
}

// Operands (n64-systemtest COP1 tests): a denormal is an Unimplemented
// Operation. A NaN with the top mantissa bit set (signalling in the legacy
// MIPS encoding) raises Invalid and the result is the default NaN; a NaN with
// that bit clear (quiet in the MIPS encoding, e.g. 0x7FBFFFFF) is an
// Unimplemented Operation.
static int s_check(H64Cpu *cpu, u32 a, u32 *cause)
{
    if (s_is_denormal(a) || (s_is_nan(a) && !s_is_snan(a))) { fpu_finish(cpu, CAUSE_E); return -1; }
    if (s_is_nan(a)) *cause |= CAUSE_V;
    return 0;
}
static int d_check(H64Cpu *cpu, u64 a, u32 *cause)
{
    if (d_is_denormal(a) || (d_is_nan(a) && !d_is_snan(a))) { fpu_finish(cpu, CAUSE_E); return -1; }
    if (d_is_nan(a)) *cause |= CAUSE_V;
    return 0;
}

// ---- Float -> integer conversions ----
// mode: -1 = current rounding mode, else H64_RM_*.
static double round_mode(double x, int mode)
{
    switch (mode)
    {
    case H64_RM_ZERO: return x < 0 ? ceil(x) : floor(x);
    case H64_RM_UP: return ceil(x);
    case H64_RM_DOWN: return floor(x);
    }
    {
        // Nearest, ties to even.
        double f = floor(x), diff = x - f;
        if (diff > 0.5) return f + 1;
        if (diff < 0.5) return f;
        return fmod(f, 2.0) == 0 ? f : f + 1;
    }
}

static int to_int(H64Cpu *cpu, double x, int isNanOrInf, int mode, int bits64, u64 *out)
{
    double r, limit = bits64 ? 9007199254740992.0 /* 2^53 */ : 2147483648.0;
    int over;
    if (mode < 0) mode = (int)(cpu->fcr31 & 3);
    if (isNanOrInf) { fpu_finish(cpu, CAUSE_E); return -1; }
    r = round_mode(x, mode);
    // 32-bit: [-2^31, 2^31); 64-bit: (-2^53, 2^53), both ends unimplemented.
    over = bits64 ? (r >= limit || r <= -limit) : (r >= limit || r < -limit);
    if (over) { fpu_finish(cpu, CAUSE_E); return -1; }
    if (fpu_finish(cpu, r != x ? CAUSE_I : 0)) return -1;
    *out = bits64 ? (u64)(s64)r : SEXT32((u32)(s32)r);
    return 0;
}

// ---- Compare ----
static void set_c(H64Cpu *cpu, int on)
{
    if (on) cpu->fcr31 |= FCR31_C;
    else cpu->fcr31 &= ~FCR31_C;
}

static void compare(H64Cpu *cpu, int cond, int unordered, int less, int equal, int snan)
{
    u32 cause = 0;
    if (unordered && ((cond & 8) || snan)) cause |= CAUSE_V;
    if (fpu_finish(cpu, cause)) return;
    set_c(cpu, ((cond & 4) && less) || ((cond & 2) && equal) || ((cond & 1) && unordered));
}

// ---- Execution ----
static void op_single(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    int fs = RD(op), ft = RT(op), fd = SA(op);
    u32 a = get_fs32(cpu, fs), b = get_ft32(cpu, ft), out;
    volatile float x = f_from(a), y = f_from(b);
    u32 cause = 0;
    switch (FUNCT(op))
    {
    case 0x00: case 0x01: case 0x02: case 0x03:   // ADD SUB MUL DIV
    {
        volatile float r;
        if (s_check(cpu, a, &cause) || s_check(cpu, b, &cause)) return;
        if (s_is_nan(a) || s_is_nan(b)) { if (!s_result(cpu, f_from(NAN_S), cause, &out)) set_result32(cpu, fd, out); return; }
        fpu_begin(cpu);
        switch (FUNCT(op))
        {
        case 0x00: r = x + y; break;
        case 0x01: r = x - y; break;
        case 0x02: r = x * y; break;
        default: r = x / y; break;
        }
        cause |= host_cause();
        if (!s_result(cpu, r, cause, &out)) set_result32(cpu, fd, out);
        return;
    }
    case 0x04:   // SQRT
    {
        volatile float r;
        if (s_check(cpu, a, &cause)) return;
        if (s_is_nan(a)) { if (!s_result(cpu, f_from(NAN_S), cause, &out)) set_result32(cpu, fd, out); return; }
        fpu_begin(cpu);
        r = sqrtf(x);
        cause |= host_cause();
        if (!s_result(cpu, r, cause, &out)) set_result32(cpu, fd, out);
        return;
    }
    case 0x05:   // ABS
        if (s_check(cpu, a, &cause)) return;
        if (s_is_nan(a)) { if (!fpu_finish(cpu, cause)) set_result32(cpu, fd, NAN_S); return; }
        if (!fpu_finish(cpu, cause)) set_result32(cpu, fd, a & 0x7FFFFFFFu);
        return;
    case 0x06: cpu->fgr[fd] = cpu->fgr[fr(cpu) ? fs : (fs & ~1)]; return;   // MOV.S copies the whole register
    case 0x07:   // NEG
        if (s_check(cpu, a, &cause)) return;
        if (s_is_nan(a)) { if (!fpu_finish(cpu, cause)) set_result32(cpu, fd, NAN_S); return; }
        if (!fpu_finish(cpu, cause)) set_result32(cpu, fd, a ^ 0x80000000u);
        return;
    case 0x08: case 0x09: case 0x0A: case 0x0B:   // ROUND/TRUNC/CEIL/FLOOR.L
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:   // ROUND/TRUNC/CEIL/FLOOR.W
    case 0x24: case 0x25:                         // CVT.W, CVT.L
    {
        static const int modes[4] = { H64_RM_NEAREST, H64_RM_ZERO, H64_RM_UP, H64_RM_DOWN };
        int f = FUNCT(op), bits64 = (f >= 0x08 && f <= 0x0B) || f == 0x25;
        int mode = f >= 0x24 ? -1 : modes[f & 3];
        u64 r;
        if (s_is_denormal(a)) { fpu_finish(cpu, CAUSE_E); return; }
        if (to_int(cpu, (double)f_from(a), s_is_nan(a) || s_is_inf(a), mode, bits64, &r)) return;
        if (bits64) set_result64(cpu, fd, r);
        else set_result32(cpu, fd, (u32)r);
        return;
    }
    case 0x21:   // CVT.D.S
    {
        u64 r;
        if (s_check(cpu, a, &cause)) return;
        if (s_is_nan(a)) { if (!d_result(cpu, d_from(NAN_D), cause, &r)) set_result64(cpu, fd, r); return; }
        fpu_begin(cpu);
        if (!d_result(cpu, (double)x, cause | host_cause(), &r)) set_result64(cpu, fd, r);
        return;
    }
    default:
        if (FUNCT(op) >= 0x30)   // C.cond.S
        {
            int un = s_is_nan(a) || s_is_nan(b);
            compare(cpu, FUNCT(op) & 15, un, !un && x < y, !un && x == y, s_is_snan(a) || s_is_snan(b));
            return;
        }
    }
    fpu_finish(cpu, CAUSE_E);   // CVT.S.S and reserved encodings
}

static void op_double(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    int fs = RD(op), ft = RT(op), fd = SA(op);
    u64 a = get_fs64(cpu, fs), b = cpu->fgr[ft], out;
    volatile double x = d_from(a), y = d_from(b);
    u32 cause = 0;
    switch (FUNCT(op))
    {
    case 0x00: case 0x01: case 0x02: case 0x03:
    {
        volatile double r;
        if (d_check(cpu, a, &cause) || d_check(cpu, b, &cause)) return;
        if (d_is_nan(a) || d_is_nan(b)) { if (!d_result(cpu, d_from(NAN_D), cause, &out)) set_result64(cpu, fd, out); return; }
        fpu_begin(cpu);
        switch (FUNCT(op))
        {
        case 0x00: r = x + y; break;
        case 0x01: r = x - y; break;
        case 0x02: r = x * y; break;
        default: r = x / y; break;
        }
        cause |= host_cause();
        if (!d_result(cpu, r, cause, &out)) set_result64(cpu, fd, out);
        return;
    }
    case 0x04:
    {
        volatile double r;
        if (d_check(cpu, a, &cause)) return;
        if (d_is_nan(a)) { if (!d_result(cpu, d_from(NAN_D), cause, &out)) set_result64(cpu, fd, out); return; }
        fpu_begin(cpu);
        r = sqrt(x);
        cause |= host_cause();
        if (!d_result(cpu, r, cause, &out)) set_result64(cpu, fd, out);
        return;
    }
    case 0x05:
        if (d_check(cpu, a, &cause)) return;
        if (d_is_nan(a)) { if (!fpu_finish(cpu, cause)) set_result64(cpu, fd, NAN_D); return; }
        if (!fpu_finish(cpu, cause)) set_result64(cpu, fd, a & 0x7FFFFFFFFFFFFFFFull);
        return;
    case 0x06: set_result64(cpu, fd, a); return;
    case 0x07:
        if (d_check(cpu, a, &cause)) return;
        if (d_is_nan(a)) { if (!fpu_finish(cpu, cause)) set_result64(cpu, fd, NAN_D); return; }
        if (!fpu_finish(cpu, cause)) set_result64(cpu, fd, a ^ 0x8000000000000000ull);
        return;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
    case 0x24: case 0x25:
    {
        static const int modes[4] = { H64_RM_NEAREST, H64_RM_ZERO, H64_RM_UP, H64_RM_DOWN };
        int f = FUNCT(op), bits64 = (f >= 0x08 && f <= 0x0B) || f == 0x25;
        int mode = f >= 0x24 ? -1 : modes[f & 3];
        u64 r;
        if (d_is_denormal(a)) { fpu_finish(cpu, CAUSE_E); return; }
        if (to_int(cpu, x, d_is_nan(a) || d_is_inf(a), mode, bits64, &r)) return;
        if (bits64) set_result64(cpu, fd, r);
        else set_result32(cpu, fd, (u32)r);
        return;
    }
    case 0x20:   // CVT.S.D
    {
        volatile float r;
        if (d_check(cpu, a, &cause)) return;
        if (d_is_nan(a)) { u32 o; if (!s_result(cpu, f_from(NAN_S), cause, &o)) set_result32(cpu, fd, o); return; }
        fpu_begin(cpu);
        r = (float)x;
        cause |= host_cause();
        {
            u32 o32;   // not through &out: a u32 written into a u64 lands in the high half on big-endian hosts
            if (!s_result(cpu, r, cause, &o32)) set_result32(cpu, fd, o32);
        }
        return;
    }
    default:
        if (FUNCT(op) >= 0x30)
        {
            int un = d_is_nan(a) || d_is_nan(b);
            compare(cpu, FUNCT(op) & 15, un, !un && x < y, !un && x == y, d_is_snan(a) || d_is_snan(b));
            return;
        }
    }
    fpu_finish(cpu, CAUSE_E);
}

// W and L sources: only conversions to S and D exist.
static void op_fixed(H64System *sys, u32 op, int isLong)
{
    H64Cpu *cpu = &sys->cpu;
    int fs = RD(op), fd = SA(op);
    s64 v = isLong ? (s64)get_fs64(cpu, fs) : (s64)(s32)get_fs32(cpu, fs);
    if (isLong && (v >= (1ll << 55) || v < -(1ll << 55))) { fpu_finish(cpu, CAUSE_E); return; }
    switch (FUNCT(op))
    {
    case 0x20:
    {
        volatile float r;
        u32 out;
        fpu_begin(cpu);
        r = (float)v;
        if (!s_result(cpu, r, host_cause(), &out)) set_result32(cpu, fd, out);
        return;
    }
    case 0x21:
    {
        volatile double r;
        u64 out;
        fpu_begin(cpu);
        r = (double)v;
        if (!d_result(cpu, r, host_cause(), &out)) set_result64(cpu, fd, out);
        return;
    }
    }
    fpu_finish(cpu, CAUSE_E);
}

void h64_cop1_execute(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    int rt = RT(op), fs = RD(op);
    switch (RS(op))
    {
    case 0x00: cpu->gpr[rt] = SEXT32(h64_fpr_get32(cpu, fs)); return;                    // MFC1
    case 0x01: cpu->gpr[rt] = h64_fpr_get64(cpu, fs); return;                            // DMFC1
    case 0x02:                                                                           // CFC1
        cpu->gpr[rt] = fs == 0 ? FCR0_VALUE : fs == 31 ? SEXT32(cpu->fcr31) : 0;
        return;
    case 0x04: h64_fpr_set32(cpu, fs, (u32)cpu->gpr[rt]); return;                        // MTC1
    case 0x05: h64_fpr_set64(cpu, fs, cpu->gpr[rt]); return;                             // DMTC1
    case 0x06:                                                                           // CTC1
        if (fs == 31)
        {
            cpu->fcr31 = (u32)cpu->gpr[rt] & FCR31_WRITABLE;
            // A cause bit written together with its enable (or E) traps at once.
            if (((cpu->fcr31 >> 12) & ((cpu->fcr31 >> 7) & 0x1F)) || (cpu->fcr31 & (CAUSE_E << 12)))
                h64_cpu_exception(cpu, EXC_FPE, 0x180);
        }
        return;
    case 0x08:                                                                           // BC1
    {
        int cond = (cpu->fcr31 & FCR31_C) != 0;
        int taken = (rt & 1) ? cond : !cond;
        if (rt & 2)
        {
            if (taken) { cpu->nextPc = cpu->curPc + 4 + (IMM16(op) << 2); cpu->branchPending = 1; }
            else { cpu->pc = cpu->nextPc; cpu->nextPc = cpu->pc + 4; }
        }
        else
        {
            if (taken) cpu->nextPc = cpu->curPc + 4 + (IMM16(op) << 2);
            cpu->branchPending = 1;
        }
        return;
    }
    case 0x03: case 0x07: fpu_finish(cpu, CAUSE_E); return;                          // DCFC1, DCTC1
    case 0x10: op_single(sys, op); return;
    case 0x11: op_double(sys, op); return;
    case 0x14: op_fixed(sys, op, 0); return;
    case 0x15: op_fixed(sys, op, 1); return;
    }
    h64_cpu_exception(cpu, EXC_RI, 0x180);
}
