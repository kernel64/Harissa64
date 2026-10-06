// Harissa64 V2 - PowerPC instruction encoder for the dynarec.
//
// 64-bit PowerPC (Xenon, and ppc64 big-endian Linux under QEMU for tests).
// Encodings from the PowerPC ISA (Book I); each function appends one 32-bit
// instruction word. Checked against GNU as in tests/unit/test_ppc_emit.cpp.
#ifndef H64_PPC_EMIT_H
#define H64_PPC_EMIT_H

#include "../common/h64_types.h"

struct H64PpcCode
{
    u32 *buf;
    u32 pos;   // in words
    u32 cap;
    int overflow;
};

static inline void ppc_put(H64PpcCode *c, u32 w)
{
    if (c->pos < c->cap) c->buf[c->pos++] = w;
    else c->overflow = 1;
}

// ---- Instruction forms ----
static inline u32 ppc_d(u32 op, u32 rt, u32 ra, u32 imm) { return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF); }
static inline u32 ppc_ds(u32 op, u32 rt, u32 ra, s32 ds, u32 xo) { return (op << 26) | (rt << 21) | (ra << 16) | ((u32)ds & 0xFFFC) | xo; }
static inline u32 ppc_x(u32 rt, u32 ra, u32 rb, u32 xo, u32 rc) { return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc; }
static inline u32 ppc_md(u32 rs, u32 ra, u32 sh, u32 mb, u32 xo)
{
    return (30u << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((((mb & 31) << 1) | (mb >> 5)) << 5) | (xo << 2) | (((sh >> 5) & 1) << 1);
}

// ---- Integer arithmetic and logic ----
static inline void ppc_addi(H64PpcCode *c, u32 rt, u32 ra, s32 imm) { ppc_put(c, ppc_d(14, rt, ra, (u32)imm)); }
static inline void ppc_addis(H64PpcCode *c, u32 rt, u32 ra, s32 imm) { ppc_put(c, ppc_d(15, rt, ra, (u32)imm)); }
static inline void ppc_li(H64PpcCode *c, u32 rt, s32 imm) { ppc_addi(c, rt, 0, imm); }
static inline void ppc_lis(H64PpcCode *c, u32 rt, s32 imm) { ppc_addis(c, rt, 0, imm); }
static inline void ppc_ori(H64PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(24, rs, ra, imm)); }
static inline void ppc_oris(H64PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(25, rs, ra, imm)); }
static inline void ppc_xori(H64PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(26, rs, ra, imm)); }
static inline void ppc_xoris(H64PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(27, rs, ra, imm)); }
static inline void ppc_andi_(H64PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(28, rs, ra, imm)); }
static inline void ppc_andis_(H64PpcCode *c, u32 ra, u32 rs, u32 imm) { ppc_put(c, ppc_d(29, rs, ra, imm)); }
static inline void ppc_mulli(H64PpcCode *c, u32 rt, u32 ra, s32 imm) { ppc_put(c, ppc_d(7, rt, ra, (u32)imm)); }
static inline void ppc_subfic(H64PpcCode *c, u32 rt, u32 ra, s32 imm) { ppc_put(c, ppc_d(8, rt, ra, (u32)imm)); }

static inline void ppc_add(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 266, 0)); }
static inline void ppc_subf(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 40, 0)); }   // rt = rb - ra
static inline void ppc_neg(H64PpcCode *c, u32 rt, u32 ra) { ppc_put(c, ppc_x(rt, ra, 0, 104, 0)); }
static inline void ppc_mullw(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 235, 0)); }
static inline void ppc_mulhw(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 75, 0)); }
static inline void ppc_mulhwu(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 11, 0)); }
static inline void ppc_mulld(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 233, 0)); }
static inline void ppc_mulhd(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 73, 0)); }
static inline void ppc_mulhdu(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 9, 0)); }
static inline void ppc_divw(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 491, 0)); }
static inline void ppc_divwu(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 459, 0)); }
static inline void ppc_divd(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 489, 0)); }
static inline void ppc_divdu(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 457, 0)); }

static inline void ppc_and(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 28, 0)); }
static inline void ppc_and_(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 28, 1)); }   // and.
static inline void ppc_andc(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 60, 0)); }
static inline void ppc_or(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 444, 0)); }
static inline void ppc_mr(H64PpcCode *c, u32 ra, u32 rs) { ppc_or(c, ra, rs, rs); }
static inline void ppc_xor(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 316, 0)); }
static inline void ppc_nor(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 124, 0)); }
static inline void ppc_extsb(H64PpcCode *c, u32 ra, u32 rs) { ppc_put(c, ppc_x(rs, ra, 0, 954, 0)); }
static inline void ppc_extsh(H64PpcCode *c, u32 ra, u32 rs) { ppc_put(c, ppc_x(rs, ra, 0, 922, 0)); }
static inline void ppc_extsw(H64PpcCode *c, u32 ra, u32 rs) { ppc_put(c, ppc_x(rs, ra, 0, 986, 0)); }

// ---- Shifts and rotates ----
static inline void ppc_slw(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 24, 0)); }
static inline void ppc_srw(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 536, 0)); }
static inline void ppc_sraw(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 792, 0)); }
static inline void ppc_srawi(H64PpcCode *c, u32 ra, u32 rs, u32 sh) { ppc_put(c, ppc_x(rs, ra, sh & 31, 824, 0)); }
static inline void ppc_sld(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 27, 0)); }
static inline void ppc_srd(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 539, 0)); }
static inline void ppc_srad(H64PpcCode *c, u32 ra, u32 rs, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 794, 0)); }
static inline void ppc_sradi(H64PpcCode *c, u32 ra, u32 rs, u32 sh)
{
    ppc_put(c, (31u << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | (413u << 2) | (((sh >> 5) & 1) << 1));
}
static inline void ppc_rlwinm(H64PpcCode *c, u32 ra, u32 rs, u32 sh, u32 mb, u32 me)
{
    ppc_put(c, (21u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1));
}
static inline void ppc_rldicl(H64PpcCode *c, u32 ra, u32 rs, u32 sh, u32 mb) { ppc_put(c, ppc_md(rs, ra, sh, mb, 0)); }
static inline void ppc_rldicr(H64PpcCode *c, u32 ra, u32 rs, u32 sh, u32 me) { ppc_put(c, ppc_md(rs, ra, sh, me, 1)); }
static inline void ppc_rldimi(H64PpcCode *c, u32 ra, u32 rs, u32 sh, u32 mb) { ppc_put(c, ppc_md(rs, ra, sh, mb, 3)); }
static inline void ppc_sldi(H64PpcCode *c, u32 ra, u32 rs, u32 n) { ppc_rldicr(c, ra, rs, n, 63 - n); }
static inline void ppc_srdi(H64PpcCode *c, u32 ra, u32 rs, u32 n) { ppc_rldicl(c, ra, rs, 64 - n, n); }
static inline void ppc_clrldi(H64PpcCode *c, u32 ra, u32 rs, u32 n) { ppc_rldicl(c, ra, rs, 0, n); }

// ---- Compares (CR field `cr`) ----
static inline void ppc_cmpw(H64PpcCode *c, u32 cr, u32 ra, u32 rb) { ppc_put(c, ppc_x(cr << 2, ra, rb, 0, 0)); }
static inline void ppc_cmpd(H64PpcCode *c, u32 cr, u32 ra, u32 rb) { ppc_put(c, ppc_x((cr << 2) | 1, ra, rb, 0, 0)); }
static inline void ppc_cmplw(H64PpcCode *c, u32 cr, u32 ra, u32 rb) { ppc_put(c, ppc_x(cr << 2, ra, rb, 32, 0)); }
static inline void ppc_cmpld(H64PpcCode *c, u32 cr, u32 ra, u32 rb) { ppc_put(c, ppc_x((cr << 2) | 1, ra, rb, 32, 0)); }
static inline void ppc_cmpwi(H64PpcCode *c, u32 cr, u32 ra, s32 imm) { ppc_put(c, ppc_d(11, cr << 2, ra, (u32)imm)); }
static inline void ppc_cmpdi(H64PpcCode *c, u32 cr, u32 ra, s32 imm) { ppc_put(c, ppc_d(11, (cr << 2) | 1, ra, (u32)imm)); }
static inline void ppc_cmplwi(H64PpcCode *c, u32 cr, u32 ra, u32 imm) { ppc_put(c, ppc_d(10, cr << 2, ra, imm)); }
static inline void ppc_cmpldi(H64PpcCode *c, u32 cr, u32 ra, u32 imm) { ppc_put(c, ppc_d(10, (cr << 2) | 1, ra, imm)); }

// ---- Loads and stores ----
static inline void ppc_lbz(H64PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_d(34, rt, ra, (u32)d)); }
static inline void ppc_lhz(H64PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_d(40, rt, ra, (u32)d)); }
static inline void ppc_lha(H64PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_d(42, rt, ra, (u32)d)); }
static inline void ppc_lwz(H64PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_d(32, rt, ra, (u32)d)); }
static inline void ppc_lwa(H64PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_ds(58, rt, ra, d, 2)); }
static inline void ppc_ld(H64PpcCode *c, u32 rt, s32 d, u32 ra) { ppc_put(c, ppc_ds(58, rt, ra, d, 0)); }
static inline void ppc_stb(H64PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_d(38, rs, ra, (u32)d)); }
static inline void ppc_sth(H64PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_d(44, rs, ra, (u32)d)); }
static inline void ppc_stw(H64PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_d(36, rs, ra, (u32)d)); }
static inline void ppc_stwu(H64PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_d(37, rs, ra, (u32)d)); }
static inline void ppc_std(H64PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_ds(62, rs, ra, d, 0)); }
static inline void ppc_stdu(H64PpcCode *c, u32 rs, s32 d, u32 ra) { ppc_put(c, ppc_ds(62, rs, ra, d, 1)); }
static inline void ppc_lbzx(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 87, 0)); }
static inline void ppc_lhzx(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 279, 0)); }
static inline void ppc_lhax(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 343, 0)); }
static inline void ppc_lwzx(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 23, 0)); }
static inline void ppc_lwax(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 341, 0)); }
static inline void ppc_ldx(H64PpcCode *c, u32 rt, u32 ra, u32 rb) { ppc_put(c, ppc_x(rt, ra, rb, 21, 0)); }
static inline void ppc_stbx(H64PpcCode *c, u32 rs, u32 ra, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 215, 0)); }
static inline void ppc_sthx(H64PpcCode *c, u32 rs, u32 ra, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 407, 0)); }
static inline void ppc_stwx(H64PpcCode *c, u32 rs, u32 ra, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 151, 0)); }
static inline void ppc_stdx(H64PpcCode *c, u32 rs, u32 ra, u32 rb) { ppc_put(c, ppc_x(rs, ra, rb, 149, 0)); }

// ---- Floating point ----
static inline u32 ppc_a(u32 op, u32 frt, u32 fra, u32 frb, u32 frc, u32 xo) { return (op << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1); }
static inline void ppc_lfs(H64PpcCode *c, u32 frt, s32 d, u32 ra) { ppc_put(c, ppc_d(48, frt, ra, (u32)d)); }
static inline void ppc_lfd(H64PpcCode *c, u32 frt, s32 d, u32 ra) { ppc_put(c, ppc_d(50, frt, ra, (u32)d)); }
static inline void ppc_stfs(H64PpcCode *c, u32 frs, s32 d, u32 ra) { ppc_put(c, ppc_d(52, frs, ra, (u32)d)); }
static inline void ppc_stfd(H64PpcCode *c, u32 frs, s32 d, u32 ra) { ppc_put(c, ppc_d(54, frs, ra, (u32)d)); }
static inline void ppc_stfiwx(H64PpcCode *c, u32 frs, u32 ra, u32 rb) { ppc_put(c, ppc_x(frs, ra, rb, 983, 0)); }
static inline void ppc_fadds(H64PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(59, t, a, b, 0, 21)); }
static inline void ppc_fsubs(H64PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(59, t, a, b, 0, 20)); }
static inline void ppc_fmuls(H64PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(59, t, a, 0, b, 25)); }
static inline void ppc_fdivs(H64PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(59, t, a, b, 0, 18)); }
static inline void ppc_fadd(H64PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(63, t, a, b, 0, 21)); }
static inline void ppc_fsub(H64PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(63, t, a, b, 0, 20)); }
static inline void ppc_fmul(H64PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(63, t, a, 0, b, 25)); }
static inline void ppc_fdiv(H64PpcCode *c, u32 t, u32 a, u32 b) { ppc_put(c, ppc_a(63, t, a, b, 0, 18)); }
static inline void ppc_frsp(H64PpcCode *c, u32 t, u32 b) { ppc_put(c, ppc_a(63, t, 0, b, 0, 12)); }
static inline void ppc_fctiw(H64PpcCode *c, u32 t, u32 b) { ppc_put(c, ppc_a(63, t, 0, b, 0, 14)); }
static inline void ppc_fctiwz(H64PpcCode *c, u32 t, u32 b) { ppc_put(c, ppc_a(63, t, 0, b, 0, 15)); }
static inline void ppc_fcfid(H64PpcCode *c, u32 t, u32 b) { ppc_put(c, (63u << 26) | (t << 21) | (b << 11) | (846u << 1)); }
static inline void ppc_fabs(H64PpcCode *c, u32 t, u32 b) { ppc_put(c, (63u << 26) | (t << 21) | (b << 11) | (264u << 1)); }
static inline void ppc_fmr(H64PpcCode *c, u32 t, u32 b) { ppc_put(c, (63u << 26) | (t << 21) | (b << 11) | (72u << 1)); }
static inline void ppc_fcmpu(H64PpcCode *c, u32 cr, u32 a, u32 b) { ppc_put(c, (63u << 26) | (cr << 23) | (a << 16) | (b << 11)); }
static inline void ppc_mffs(H64PpcCode *c, u32 t) { ppc_put(c, (63u << 26) | (t << 21) | (583u << 1)); }

// ---- Special registers and branches ----
static inline u32 ppc_spr(u32 spr) { return ((spr & 31) << 16) | ((spr >> 5) << 11); }
static inline void ppc_mflr(H64PpcCode *c, u32 rt) { ppc_put(c, (31u << 26) | (rt << 21) | ppc_spr(8) | (339u << 1)); }
static inline void ppc_mtlr(H64PpcCode *c, u32 rs) { ppc_put(c, (31u << 26) | (rs << 21) | ppc_spr(8) | (467u << 1)); }
static inline void ppc_mtctr(H64PpcCode *c, u32 rs) { ppc_put(c, (31u << 26) | (rs << 21) | ppc_spr(9) | (467u << 1)); }
static inline void ppc_blr(H64PpcCode *c) { ppc_put(c, 0x4E800020u); }
static inline void ppc_bctr(H64PpcCode *c) { ppc_put(c, 0x4E800420u); }
static inline void ppc_bctrl(H64PpcCode *c) { ppc_put(c, 0x4E800421u); }
static inline void ppc_nop(H64PpcCode *c) { ppc_put(c, 0x60000000u); }

// Conditions on a CR field: BO 12 = branch if the bit is set, 4 = if clear.
enum { PPC_LT = 0, PPC_GT = 1, PPC_EQ = 2, PPC_UN = 3 };   // UN: unordered after fcmpu
// Conditional branch to be patched: returns the word index.
static inline u32 ppc_bc_fwd(H64PpcCode *c, u32 bo, u32 cr, u32 bit)
{
    u32 at = c->pos;
    ppc_put(c, (16u << 26) | (bo << 21) | ((cr * 4 + bit) << 16));
    return at;
}
static inline u32 ppc_b_fwd(H64PpcCode *c)
{
    u32 at = c->pos;
    ppc_put(c, 18u << 26);
    return at;
}
// Points the branch at word `at` to the current position.
static inline void ppc_patch_here(H64PpcCode *c, u32 at)
{
    s32 off = (s32)(c->pos - at) * 4;
    if (at >= c->cap) return;
    if ((c->buf[at] >> 26) == 16) c->buf[at] = (c->buf[at] & ~0xFFFCu) | ((u32)off & 0xFFFCu);
    else c->buf[at] = (c->buf[at] & ~0x03FFFFFCu) | ((u32)off & 0x03FFFFFCu);
}

// Loads any 64-bit constant (5 instructions at most).
void ppc_li64(H64PpcCode *c, u32 rt, u64 v);
// Loads a 32-bit value zero-extended (lis/ori, then clears the upper half when needed).
void ppc_li32u(H64PpcCode *c, u32 rt, u32 v);

#endif
