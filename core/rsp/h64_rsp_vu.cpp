// Harissa64 V2 - RSP vector unit (COP2) and vector loads/stores (LWC2/SWC2).
// Ported from ares (ISC licence): ares/n64/rsp/interpreter-vpu.cpp, the
// SISD (scalar) paths, and the decode tables of interpreter.cpp
// (commit a776c509). See THIRD_PARTY.md.
//
// A vector register is 8 elements of 16 bits; element 0 holds bytes 0 and 1
// in memory order (byte 0 is its high byte). Flag registers hold 0 or 0xFFFF
// per element.
#include "h64_rsp.h"

#include <string.h>

#include "../system/h64_system.h"

typedef u16 Vec[8];

// ---- Element and byte access ----
static u8 vbyte(const u16 *v, u32 i)
{
    i &= 15;
    return (u8)(i & 1 ? v[i >> 1] : v[i >> 1] >> 8);
}

static void vset_byte(u16 *v, u32 i, u8 b)
{
    i &= 15;
    if (i & 1) v[i >> 1] = (u16)((v[i >> 1] & 0xFF00) | b);
    else v[i >> 1] = (u16)((v[i >> 1] & 0x00FF) | (b << 8));
}

// Element selector e (0..15) applied to vt.
static void vsel(u16 *out, const u16 *in, u32 e)
{
    u32 n;
    for (n = 0; n < 8; n++)
    {
        u32 src;
        if (e < 2) src = n;
        else if (e == 2) src = n & ~1u;
        else if (e == 3) src = n | 1u;
        else if (e < 8) src = (n & 4u) | (e - 4);
        else src = e - 8;
        out[n] = in[src];
    }
}

static int flag(const u16 *f, u32 n) { return f[n] != 0; }
static int set_flag(u16 *f, u32 n, int v) { f[n] = (u16)(v ? 0xFFFF : 0); return v; }
static void clear_flags(u16 *f) { memset(f, 0, 8 * sizeof(u16)); }

static s32 sclamp16(s64 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (s32)v; }
static s64 sclip48(s64 v) { return (s64)((u64)v << 16) >> 16; }

static u32 clz32(u32 v)
{
    u32 n = 0;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

// ---- DMEM ----
static u8 dm_r(H64System *sys, u32 a) { return sys->spMem[a & 0xFFF]; }
static void dm_w(H64System *sys, u32 a, u8 v) { sys->spMem[a & 0xFFF] = v; }

// ---- Accumulator ----
static u64 acc_get(const H64Rsp *r, u32 n)
{
    return (u64)r->acch[n] << 32 | (u64)r->accm[n] << 16 | r->accl[n];
}

static void acc_set(H64Rsp *r, u32 n, u64 v)
{
    r->acch[n] = (u16)(v >> 32);
    r->accm[n] = (u16)(v >> 16);
    r->accl[n] = (u16)v;
}

static u16 acc_saturate(const H64Rsp *r, u32 n, int slice, u16 negative, u16 positive)
{
    if ((s16)r->acch[n] < 0)
    {
        if (r->acch[n] != 0xFFFF) return negative;
        if ((s16)r->accm[n] >= 0) return negative;
    }
    else
    {
        if (r->acch[n] != 0x0000) return positive;
        if ((s16)r->accm[n] < 0) return positive;
    }
    return slice ? r->accm[n] : r->accl[n];
}

void h64_rsp_vu_init_tables(H64Rsp *rsp)
{
    u32 index;
    rsp->reciprocals[0] = 0xFFFF;
    for (index = 1; index < 512; index++)
    {
        u64 a = index + 512;
        u64 b = (1ull << 34) / a;
        rsp->reciprocals[index] = (u16)((b + 1) >> 8);
    }
    for (index = 0; index < 512; index++)
    {
        u64 a = (u64)(index + 512) >> (index % 2 == 1 ? 1 : 0);
        u64 b = 1ull << 17;
        // the largest b where b < 1.0 / sqrt(a)
        while (a * (b + 1) * (b + 1) < (1ull << 44)) b++;
        rsp->inverseSquareRoots[index] = (u16)(b >> 1);
    }
}

// ---- COP2 moves ----
static void flag_pair(H64Rsp *r, u32 rd, u16 **hi, u16 **lo)
{
    switch (rd & 3)
    {
    case 0: *hi = r->vcoh; *lo = r->vcol; break;
    case 1: *hi = r->vcch; *lo = r->vccl; break;
    default: *hi = 0; *lo = r->vce; break;   // 3: unverified (ares)
    }
}

static void cfc2(H64Rsp *r, u32 rt, u32 rd)
{
    u16 *hi, *lo;
    u32 v = 0, n;
    flag_pair(r, rd, &hi, &lo);
    for (n = 0; n < 8; n++)
    {
        v |= (u32)flag(lo, n) << n;
        if (hi) v |= (u32)flag(hi, n) << (8 + n);
    }
    if (rt) r->r[rt] = (u32)(s32)(s16)v;
}

static void ctc2(H64Rsp *r, u32 value, u32 rd)
{
    u16 *hi, *lo;
    u32 n;
    flag_pair(r, rd, &hi, &lo);
    for (n = 0; n < 8; n++)
    {
        set_flag(lo, n, (value >> n) & 1);
        if (hi) set_flag(hi, n, (value >> (8 + n)) & 1);
    }
}

// ---- Computational instructions ----
static void op_vector(H64Rsp *r, u32 op)
{
    u32 e = (op >> 21) & 15, vtn = (op >> 16) & 31, vsn = (op >> 11) & 31, vdn = (op >> 6) & 31, de = (op >> 11) & 7;
    u16 *vd = r->vr[vdn];
    Vec vs, vt, vte, res;
    u32 n;
    memcpy(vs, r->vr[vsn], sizeof(Vec));
    memcpy(vt, r->vr[vtn], sizeof(Vec));
    vsel(vte, vt, e);

    switch (op & 0x3F)
    {
    case 0x00: case 0x01:   // VMULF, VMULU
        for (n = 0; n < 8; n++)
        {
            acc_set(r, n, (u64)((s64)(s16)vs[n] * (s64)(s16)vte[n] * 2 + 0x8000));
            if ((op & 0x3F) == 0x00) res[n] = acc_saturate(r, n, 1, 0x8000, 0x7FFF);
            else res[n] = (s16)r->acch[n] < 0 ? 0x0000 : ((s16)r->acch[n] ^ (s16)r->accm[n]) < 0 ? 0xFFFF : r->accm[n];
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x02: case 0x0A:   // VRNDP, VRNDN
        for (n = 0; n < 8; n++)
        {
            s32 product = (s16)vte[n];
            s64 acc;
            if (vsn & 1) product = (s32)((u32)product << 16);
            acc = sclip48((s64)acc_get(r, n));
            if ((op & 0x3F) == 0x0A && acc < 0) acc = sclip48(acc + product);
            if ((op & 0x3F) == 0x02 && acc >= 0) acc = sclip48(acc + product);
            acc_set(r, n, (u64)acc);
            res[n] = (u16)sclamp16(acc >> 16);
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x03:   // VMULQ
        for (n = 0; n < 8; n++)
        {
            s32 product = (s32)(s16)vs[n] * (s32)(s16)vte[n];
            if (product < 0) product += 31;   // round
            r->acch[n] = (u16)(product >> 16);
            r->accm[n] = (u16)product;
            r->accl[n] = 0;
            res[n] = (u16)(sclamp16(product >> 1) & ~15);
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x04:   // VMUDL
        for (n = 0; n < 8; n++) acc_set(r, n, (u16)(((u32)vs[n] * (u32)vte[n]) >> 16));
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x05:   // VMUDM
        for (n = 0; n < 8; n++) acc_set(r, n, (u64)(s64)((s32)(s16)vs[n] * (s32)vte[n]));
        memcpy(vd, r->accm, sizeof(Vec));
        return;
    case 0x06:   // VMUDN
        for (n = 0; n < 8; n++) acc_set(r, n, (u64)(s64)((s32)vs[n] * (s32)(s16)vte[n]));
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x07:   // VMUDH
        for (n = 0; n < 8; n++)
        {
            acc_set(r, n, (u64)((s64)((s32)(s16)vs[n] * (s32)(s16)vte[n]) << 16));
            res[n] = acc_saturate(r, n, 1, 0x8000, 0x7FFF);
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x08: case 0x09:   // VMACF, VMACU
        for (n = 0; n < 8; n++)
        {
            acc_set(r, n, acc_get(r, n) + (u64)((s64)(s16)vs[n] * (s64)(s16)vte[n] * 2));
            if ((op & 0x3F) == 0x08) res[n] = acc_saturate(r, n, 1, 0x8000, 0x7FFF);
            else res[n] = (s16)r->acch[n] < 0 ? 0x0000 : ((s16)r->acch[n] || (s16)r->accm[n] < 0) ? 0xFFFF : r->accm[n];
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x0B:   // VMACQ
        for (n = 0; n < 8; n++)
        {
            s32 product = (s32)((u32)r->acch[n] << 16 | r->accm[n]);
            if (product < 0 && !(product & (1 << 5))) product += 32;
            else if (product >= 32 && !(product & (1 << 5))) product -= 32;
            r->acch[n] = (u16)(product >> 16);
            r->accm[n] = (u16)product;
            vd[n] = (u16)(sclamp16(product >> 1) & ~15);
        }
        return;
    case 0x0C:   // VMADL
        for (n = 0; n < 8; n++)
        {
            acc_set(r, n, acc_get(r, n) + (((u32)vs[n] * (u32)vte[n]) >> 16));
            res[n] = acc_saturate(r, n, 0, 0x0000, 0xFFFF);
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x0D:   // VMADM
        for (n = 0; n < 8; n++)
        {
            acc_set(r, n, acc_get(r, n) + (u64)(s64)((s32)(s16)vs[n] * (s32)vte[n]));
            res[n] = acc_saturate(r, n, 1, 0x8000, 0x7FFF);
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x0E:   // VMADN
        for (n = 0; n < 8; n++)
        {
            acc_set(r, n, acc_get(r, n) + (u64)(s64)((s32)vs[n] * (s32)(s16)vte[n]));
            res[n] = acc_saturate(r, n, 0, 0x0000, 0xFFFF);
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x0F:   // VMADH
        for (n = 0; n < 8; n++)
        {
            s32 result = (s32)(u32)(acc_get(r, n) >> 16) + (s32)(s16)vs[n] * (s32)(s16)vte[n];
            r->acch[n] = (u16)((u32)result >> 16);
            r->accm[n] = (u16)result;
            res[n] = acc_saturate(r, n, 1, 0x8000, 0x7FFF);
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x10:   // VADD
        for (n = 0; n < 8; n++)
        {
            s32 result = (s16)vs[n] + (s16)vte[n] + flag(r->vcol, n);
            r->accl[n] = (u16)result;
            res[n] = (u16)sclamp16(result);
        }
        clear_flags(r->vcol);
        clear_flags(r->vcoh);
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x11:   // VSUB
        for (n = 0; n < 8; n++)
        {
            s32 result = (s16)vs[n] - (s16)vte[n] - flag(r->vcol, n);
            r->accl[n] = (u16)result;
            res[n] = (u16)sclamp16(result);
        }
        clear_flags(r->vcol);
        clear_flags(r->vcoh);
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x13:   // VABS
        for (n = 0; n < 8; n++)
        {
            if ((s16)vs[n] < 0)
            {
                if ((s16)vte[n] == -32768) { r->accl[n] = 0x8000; res[n] = 0x7FFF; }
                else { r->accl[n] = (u16)-(s16)vte[n]; res[n] = r->accl[n]; }
            }
            else if ((s16)vs[n] > 0) { r->accl[n] = vte[n]; res[n] = vte[n]; }
            else { r->accl[n] = 0; res[n] = 0; }
        }
        memcpy(vd, res, sizeof(Vec));
        return;
    case 0x14:   // VADDC
        for (n = 0; n < 8; n++)
        {
            u32 result = (u32)vs[n] + vte[n];
            r->accl[n] = (u16)result;
            set_flag(r->vcol, n, (result >> 16) != 0);
        }
        clear_flags(r->vcoh);
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x15:   // VSUBC
        for (n = 0; n < 8; n++)
        {
            u32 result = (u32)vs[n] - vte[n];
            r->accl[n] = (u16)result;
            set_flag(r->vcol, n, (result >> 16) != 0);
            set_flag(r->vcoh, n, result != 0);
        }
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x1D:   // VSAR
        if (e == 8) memcpy(vd, r->acch, sizeof(Vec));
        else if (e == 9) memcpy(vd, r->accm, sizeof(Vec));
        else if (e == 10) memcpy(vd, r->accl, sizeof(Vec));
        else memset(vd, 0, sizeof(Vec));
        return;
    case 0x20:   // VLT
        for (n = 0; n < 8; n++)
            r->accl[n] = set_flag(r->vccl, n, (s16)vs[n] < (s16)vte[n] ||
                                  ((s16)vs[n] == (s16)vte[n] && flag(r->vcol, n) && flag(r->vcoh, n))) ? vs[n] : vte[n];
        clear_flags(r->vcch);
        clear_flags(r->vcol);
        clear_flags(r->vcoh);
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x21:   // VEQ
        for (n = 0; n < 8; n++)
            r->accl[n] = set_flag(r->vccl, n, !flag(r->vcoh, n) && vs[n] == vte[n]) ? vs[n] : vte[n];
        clear_flags(r->vcch);   // unverified (ares)
        clear_flags(r->vcol);
        clear_flags(r->vcoh);
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x22:   // VNE
        for (n = 0; n < 8; n++)
            r->accl[n] = set_flag(r->vccl, n, vs[n] != vte[n] || flag(r->vcoh, n)) ? vs[n] : vte[n];
        clear_flags(r->vcch);   // unverified (ares)
        clear_flags(r->vcol);
        clear_flags(r->vcoh);
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x23:   // VGE
        for (n = 0; n < 8; n++)
            r->accl[n] = set_flag(r->vccl, n, (s16)vs[n] > (s16)vte[n] ||
                                  ((s16)vs[n] == (s16)vte[n] && (!flag(r->vcol, n) || !flag(r->vcoh, n)))) ? vs[n] : vte[n];
        clear_flags(r->vcch);   // unverified (ares)
        clear_flags(r->vcol);
        clear_flags(r->vcoh);
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x24:   // VCL
        for (n = 0; n < 8; n++)
        {
            if (flag(r->vcol, n))
            {
                if (flag(r->vcoh, n))
                    r->accl[n] = flag(r->vccl, n) ? (u16)-(s32)vte[n] : vs[n];
                else
                {
                    u16 sum = (u16)(vs[n] + vte[n]);
                    int carry = ((u32)vs[n] + vte[n]) != sum;
                    if (flag(r->vce, n))
                        r->accl[n] = set_flag(r->vccl, n, !sum || !carry) ? (u16)-(s32)vte[n] : vs[n];
                    else
                        r->accl[n] = set_flag(r->vccl, n, !sum && !carry) ? (u16)-(s32)vte[n] : vs[n];
                }
            }
            else
            {
                if (flag(r->vcoh, n))
                    r->accl[n] = flag(r->vcch, n) ? vte[n] : vs[n];
                else
                    r->accl[n] = set_flag(r->vcch, n, (s32)vs[n] - (s32)vte[n] >= 0) ? vte[n] : vs[n];
            }
        }
        clear_flags(r->vcol);
        clear_flags(r->vcoh);
        clear_flags(r->vce);
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x25:   // VCH
        for (n = 0; n < 8; n++)
        {
            s16 s = (s16)vs[n], t = (s16)vte[n];
            if ((s ^ t) < 0)
            {
                s16 result = (s16)(s + t);
                r->accl[n] = result <= 0 ? (u16)-t : (u16)s;
                set_flag(r->vccl, n, result <= 0);
                set_flag(r->vcch, n, t < 0);
                set_flag(r->vcol, n, 1);
                set_flag(r->vcoh, n, result != 0 && vs[n] != (vte[n] ^ 0xFFFF));
                set_flag(r->vce, n, result == -1);
            }
            else
            {
                s16 result = (s16)(s - t);
                r->accl[n] = result >= 0 ? (u16)t : (u16)s;
                set_flag(r->vccl, n, t < 0);
                set_flag(r->vcch, n, result >= 0);
                set_flag(r->vcol, n, 0);
                set_flag(r->vcoh, n, result != 0 && vs[n] != (vte[n] ^ 0xFFFF));
                set_flag(r->vce, n, 0);
            }
        }
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x26:   // VCR
        for (n = 0; n < 8; n++)
        {
            s16 s = (s16)vs[n], t = (s16)vte[n];
            if ((s ^ t) < 0)
            {
                set_flag(r->vcch, n, t < 0);
                r->accl[n] = set_flag(r->vccl, n, s + t + 1 <= 0) ? (u16)~vte[n] : vs[n];
            }
            else
            {
                set_flag(r->vccl, n, t < 0);
                r->accl[n] = set_flag(r->vcch, n, s - t >= 0) ? vte[n] : vs[n];
            }
        }
        clear_flags(r->vcol);
        clear_flags(r->vcoh);
        clear_flags(r->vce);
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x27:   // VMRG
        for (n = 0; n < 8; n++) r->accl[n] = flag(r->vccl, n) ? vs[n] : vte[n];
        clear_flags(r->vcoh);
        clear_flags(r->vcol);
        memcpy(vd, r->accl, sizeof(Vec));
        return;
    case 0x28: for (n = 0; n < 8; n++) r->accl[n] = vs[n] & vte[n]; memcpy(vd, r->accl, sizeof(Vec)); return;             // VAND
    case 0x29: for (n = 0; n < 8; n++) r->accl[n] = (u16)~(vs[n] & vte[n]); memcpy(vd, r->accl, sizeof(Vec)); return;    // VNAND
    case 0x2A: for (n = 0; n < 8; n++) r->accl[n] = vs[n] | vte[n]; memcpy(vd, r->accl, sizeof(Vec)); return;             // VOR
    case 0x2B: for (n = 0; n < 8; n++) r->accl[n] = (u16)~(vs[n] | vte[n]); memcpy(vd, r->accl, sizeof(Vec)); return;    // VNOR
    case 0x2C: for (n = 0; n < 8; n++) r->accl[n] = vs[n] ^ vte[n]; memcpy(vd, r->accl, sizeof(Vec)); return;             // VXOR
    case 0x2D: for (n = 0; n < 8; n++) r->accl[n] = (u16)~(vs[n] ^ vte[n]); memcpy(vd, r->accl, sizeof(Vec)); return;    // VNXOR
    case 0x30: case 0x31: case 0x34: case 0x35:   // VRCP, VRCPL, VRSQ, VRSQL
    {
        int isLong = (op & 1) != 0, isRsq = (op & 4) != 0;
        s32 input = isLong && r->divdp ? (s32)((u32)(u16)r->divin << 16 | vt[e & 7]) : (s32)(s16)vt[e & 7];
        s32 mask = input >> 31, result;
        u32 data = (u32)(input ^ mask);
        if (input > -32768) data -= (u32)mask;
        if (data == 0) result = 0x7FFFFFFF;
        else if (input == -32768) result = (s32)0xFFFF0000u;
        else
        {
            u32 shift = clz32(data);
            u32 index = (u32)((((u64)data << shift) & 0x7FC00000u) >> 22);
            if (isRsq)
            {
                result = r->inverseSquareRoots[(index & 0x1FE) | (shift & 1)];
                result = (0x10000 | result) << 14;
                result = (result >> ((31 - shift) >> 1)) ^ mask;
            }
            else
            {
                result = r->reciprocals[index];
                result = (0x10000 | result) << 14;
                result = (result >> (31 - shift)) ^ mask;
            }
        }
        r->divdp = 0;
        r->divout = (s16)(result >> 16);
        memcpy(r->accl, vte, sizeof(Vec));
        vd[de] = (u16)result;
        return;
    }
    case 0x32: case 0x36:   // VRCPH, VRSQH
        memcpy(r->accl, vte, sizeof(Vec));
        r->divdp = 1;
        r->divin = (s16)vt[e & 7];
        vd[de] = (u16)r->divout;
        return;
    case 0x33:   // VMOV
        vd[de] = vte[de];
        memcpy(r->accl, vte, sizeof(Vec));
        return;
    case 0x37: case 0x3F:   // VNOP, VNULL
        return;
    default:   // unimplemented encodings: VZERO (accumulator = vs + vt, result 0)
        for (n = 0; n < 8; n++) r->accl[n] = (u16)((s16)vs[n] + (s16)vte[n]);
        memset(vd, 0, sizeof(Vec));
        return;
    }
}

void h64_rsp_cop2(H64System *sys, u32 op)
{
    H64Rsp *r = &sys->rsp;
    u32 rt = (op >> 16) & 31, rd = (op >> 11) & 31, e = (op >> 7) & 15;
    if (op & 0x02000000u)
    {
        op_vector(r, op);
        return;
    }
    switch ((op >> 21) & 31)
    {
    case 0x00:   // MFC2
        if (rt) r->r[rt] = (u32)(s32)(s16)((u16)vbyte(r->vr[rd], e) << 8 | vbyte(r->vr[rd], e + 1));
        return;
    case 0x02: cfc2(r, rt, rd); return;
    case 0x04:   // MTC2
        vset_byte(r->vr[rd], e, (u8)(r->r[rt] >> 8));
        if (e != 15) vset_byte(r->vr[rd], e + 1, (u8)r->r[rt]);
        return;
    case 0x06: ctc2(r, r->r[rt], rd); return;
    }
}

// ---- Loads ----
void h64_rsp_lwc2(H64System *sys, u32 op)
{
    H64Rsp *r = &sys->rsp;
    u32 vtn = (op >> 16) & 31, e = (op >> 7) & 15;
    s32 imm = (s32)((op & 0x7F) ^ 0x40) - 0x40;   // sign-extended 7 bits
    u32 base = r->r[(op >> 21) & 31];
    u16 *vt = r->vr[vtn];
    u32 address, offset;
    switch ((op >> 11) & 31)
    {
    case 0x00:   // LBV
        vset_byte(vt, e, dm_r(sys, base + imm));
        return;
    case 0x01: case 0x02: case 0x03:   // LSV, LLV, LDV
    {
        u32 size = 1u << ((op >> 11) & 31);   // 2, 4, 8
        u32 end = e + size < 16 ? e + size : 16;
        address = base + imm * size;
        for (offset = e; offset < end; offset++) vset_byte(vt, offset, dm_r(sys, address++));
        return;
    }
    case 0x04:   // LQV
    {
        u32 end;
        address = base + imm * 16;
        end = 16 + e - (address & 15);
        if (end > 16) end = 16;
        for (offset = e; offset < end; offset++) vset_byte(vt, offset, dm_r(sys, address++));
        return;
    }
    case 0x05:   // LRV
    {
        s32 start;
        address = base + imm * 16;
        start = 16 - ((s32)(address & 15) - (s32)e);
        address &= ~15u;
        for (; start < 16; start++) vset_byte(vt, (u32)start, dm_r(sys, address++));
        return;
    }
    case 0x06: case 0x07:   // LPV, LUV
    {
        u32 index, shift = ((op >> 11) & 31) == 0x06 ? 8 : 7;
        address = base + imm * 8;
        index = (address & 7) - e;
        address &= ~7u;
        for (offset = 0; offset < 8; offset++)
            vt[offset] = (u16)(dm_r(sys, address + ((index + offset) & 15)) << shift);
        return;
    }
    case 0x08:   // LHV
    {
        u32 index;
        address = base + imm * 16;
        index = (address & 7) - e;
        address &= ~7u;
        for (offset = 0; offset < 8; offset++)
            vt[offset] = (u16)(dm_r(sys, address + ((index + offset * 2) & 15)) << 7);
        return;
    }
    case 0x09:   // LFV
    {
        u32 index, end = e + 8 < 16 ? e + 8 : 16;
        Vec tmp;
        address = base + imm * 16;
        index = (address & 7) - e;
        address &= ~7u;
        for (offset = 0; offset < 4; offset++)
        {
            tmp[offset + 0] = (u16)(dm_r(sys, address + ((index + offset * 4 + 0) & 15)) << 7);
            tmp[offset + 4] = (u16)(dm_r(sys, address + ((index + offset * 4 + 8) & 15)) << 7);
        }
        for (offset = e; offset < end; offset++) vset_byte(vt, offset, vbyte(tmp, offset));
        return;
    }
    case 0x0B:   // LTV
    {
        u32 begin, vtbase = vtn & ~7u, vtoff = e >> 1, i;
        address = base + imm * 16;
        begin = address & ~7u;
        address = begin + ((e + (address & 8)) & 15);
        for (i = 0; i < 8; i++)
        {
            vset_byte(r->vr[vtbase + vtoff], i * 2 + 0, dm_r(sys, address++));
            if (address == begin + 16) address = begin;
            vset_byte(r->vr[vtbase + vtoff], i * 2 + 1, dm_r(sys, address++));
            if (address == begin + 16) address = begin;
            vtoff = (vtoff + 1) & 7;
        }
        return;
    }
    }
    // 0x0A (LWV) does not exist on the N64 RSP; other encodings do nothing.
}

// ---- Stores ----
void h64_rsp_swc2(H64System *sys, u32 op)
{
    H64Rsp *r = &sys->rsp;
    u32 vtn = (op >> 16) & 31, e = (op >> 7) & 15;
    s32 imm = (s32)((op & 0x7F) ^ 0x40) - 0x40;
    u32 base = r->r[(op >> 21) & 31];
    const u16 *vt = r->vr[vtn];
    u32 address, offset;
    switch ((op >> 11) & 31)
    {
    case 0x00:   // SBV
        dm_w(sys, base + imm, vbyte(vt, e));
        return;
    case 0x01: case 0x02: case 0x03:   // SSV, SLV, SDV
    {
        u32 size = 1u << ((op >> 11) & 31);
        address = base + imm * size;
        for (offset = e; offset < e + size; offset++) dm_w(sys, address++, vbyte(vt, offset));
        return;
    }
    case 0x04:   // SQV
    {
        u32 end;
        address = base + imm * 16;
        end = e + (16 - (address & 15));
        for (offset = e; offset < end; offset++) dm_w(sys, address++, vbyte(vt, offset));
        return;
    }
    case 0x05:   // SRV
    {
        u32 end, b;
        address = base + imm * 16;
        end = e + (address & 15);
        b = 16 - (address & 15);
        address &= ~15u;
        for (offset = e; offset < end; offset++) dm_w(sys, address++, vbyte(vt, offset + b));
        return;
    }
    case 0x06:   // SPV
        address = base + imm * 8;
        for (offset = e; offset < e + 8; offset++)
        {
            if ((offset & 15) < 8) dm_w(sys, address++, vbyte(vt, (offset & 7) << 1));
            else dm_w(sys, address++, (u8)(vt[offset & 7] >> 7));
        }
        return;
    case 0x07:   // SUV
        address = base + imm * 8;
        for (offset = e; offset < e + 8; offset++)
        {
            if ((offset & 15) < 8) dm_w(sys, address++, (u8)(vt[offset & 7] >> 7));
            else dm_w(sys, address++, vbyte(vt, (offset & 7) << 1));
        }
        return;
    case 0x08:   // SHV
    {
        u32 index;
        address = base + imm * 16;
        index = address & 7;
        address &= ~7u;
        for (offset = 0; offset < 8; offset++)
        {
            u32 b = e + offset * 2;
            u8 value = (u8)(vbyte(vt, b) << 1 | vbyte(vt, b + 1) >> 7);
            dm_w(sys, address + ((index + offset * 2) & 15), value);
        }
        return;
    }
    case 0x09:   // SFV
    {
        static const s8 order[16][4] = {
            { 0, 1, 2, 3 }, { 6, 7, 4, 5 }, { -1 }, { -1 }, { 1, 2, 3, 0 }, { 7, 4, 5, 6 }, { -1 }, { -1 },
            { 4, 5, 6, 7 }, { -1 }, { -1 }, { 3, 0, 1, 2 }, { 5, 6, 7, 4 }, { -1 }, { -1 }, { 0, 1, 2, 3 } };
        u32 b, i;
        address = base + imm * 16;
        b = address & 7;
        address &= ~7u;
        for (i = 0; i < 4; i++)
        {
            s8 el = order[e][0] < 0 ? -1 : order[e][i];
            dm_w(sys, address + ((b + i * 4) & 15), el < 0 ? 0 : (u8)(vt[el] >> 7));
        }
        return;
    }
    case 0x0A:   // SWV
    {
        u32 b;
        address = base + imm * 16;
        b = address & 7;
        address &= ~7u;
        for (offset = e; offset < e + 16; offset++) dm_w(sys, address + (b++ & 15), vbyte(vt, offset));
        return;
    }
    case 0x0B:   // STV
    {
        u32 start = vtn & ~7u, end = start + 8, element = 16 - (e & ~1u), b;
        address = base + imm * 16;
        b = (address & 7) - (e & ~1u);
        address &= ~7u;
        for (offset = start; offset < end; offset++)
        {
            dm_w(sys, address + (b++ & 15), vbyte(r->vr[offset], element++));
            dm_w(sys, address + (b++ & 15), vbyte(r->vr[offset], element++));
        }
        return;
    }
    }
}
