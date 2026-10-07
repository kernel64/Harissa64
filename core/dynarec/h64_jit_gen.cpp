// Harissa64 V2 - recompiler code generation.
//
// Block layout (64-bit PowerPC):
//   prologue: stdu/stwu r1,-FRAME(r1); mflr r0; save r0 and r24..r31 in the
//             top of the frame; r31 = sys, r30 = &sys->cpu, r29 = RDRAM,
//             r28 = the interpreter helper
//   body:     one sequence per MIPS instruction, native or a call to the
//             reference interpreter for that instruction
//   exit:     restore and blr
// The frame keeps the 112-byte ELFv1 header and parameter area at its
// bottom, where called C functions save their link register and TOC on
// ppc64 Linux; Xbox functions save theirs below their own stack pointer.
//
// Accounting: native instructions don't touch H64Cpu's pc, cycles or
// instruction count. The generator counts them (`pending`) and writes the
// state back ("sync") before calling the interpreter and when leaving the
// block, so the interpreter always sees exactly the state it would have
// produced itself. Native code is only generated for blocks compiled in
// kernel mode (where games run); COP0 instructions end a block, so the mode
// can't change inside one.
#include "h64_jit.h"
#include "h64_jit_internal.h"
#include "h64_ppc_emit.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_fenv.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"

#define FRAME 192
#define MAX_BLOCK_BYTES (H64_JIT_MAX_INSNS * 320 + 2048)

// Registers: r31 sys, r30 cpu, r29 RDRAM, r28 helper, r27 branch taken, r26 jump target.
enum { R_COND = 27, R_TARGET = 26 };

#define OFF_GPR(i) ((s32)(offsetof(H64Cpu, gpr) + 8 * (i)))
#define OFF_HI ((s32)offsetof(H64Cpu, hi))
#define OFF_LO ((s32)offsetof(H64Cpu, lo))
#define OFF_PC ((s32)offsetof(H64Cpu, pc))
#define OFF_NEXTPC ((s32)offsetof(H64Cpu, nextPc))
#define OFF_BRANCH ((s32)offsetof(H64Cpu, branchPending))
#define OFF_CYCLES ((s32)offsetof(H64Cpu, cycles))
#define OFF_INSNS ((s32)offsetof(H64Cpu, instructions))
#define OFF_FGR(i) ((s32)(offsetof(H64Cpu, fgr) + 8 * (i)))
#define OFF_COP0(i) ((s32)(offsetof(H64Cpu, cop0) + 8 * (i)))
#define OFF_FCR31 ((s32)offsetof(H64Cpu, fcr31))
#define OFF_FPMIN(dbl) ((s32)(offsetof(H64Cpu, jitFpMin) + 8 * (dbl)))
#define OFF_JITSCRATCH ((s32)offsetof(H64Cpu, jitScratch))

static u64 sext32(u32 v) { return (u64)(s64)(s32)v; }

// ---- Helpers called from generated code ----

// Runs the instruction at cpu->pc with the reference interpreter. Returns
// non-zero when the block must stop: an exception, a pc other than the
// next one in the block (taken branch, nullified delay slot, end of the
// delay slot), or one of h64_jit_should_exit's reasons.
static int helper_interp(H64System *sys, u32 expectedNext, u32 op, u32 haveOp)
{
    H64Cpu *cpu = &sys->cpu;
    sys->jit->stats.helperCalls++;
    if (sys->jit->opHist)
    {
        // Why a load or store left the fast path: 192 not KSEG0/1, 193 unaligned,
        // 194 outside RDRAM, 195 store to a page with code, 196 other.
        u32 paddr0;
        u32 opw = 0, opc;
        if (haveOp) opw = op;
        if (haveOp || h64_cpu_peek_op(sys, &opw) == 0)
        {
            opc = opw >> 26;
            if ((opc >= 0x20 && opc <= 0x27) || (opc >= 0x28 && opc <= 0x2B) || opc == 0x37 || opc == 0x3F)
            {
                u64 a = cpu->gpr[(opw >> 21) & 31] + (u64)(s64)(s16)(opw & 0xFFFF);
                u32 size = (opc & 3) == 0 ? 1 : (opc & 3) == 1 ? 2 : (opc == 0x37 || opc == 0x3F) ? 8 : 4;
                if ((u64)(s64)(s32)a != a || ((u32)a >> 30) != 2) sys->jit->opHist[192]++;
                else if (a & (size - 1)) sys->jit->opHist[193]++;
                else if ((paddr0 = (u32)a & 0x1FFFFFFF) >= H64_RDRAM_SIZE) sys->jit->opHist[194]++;
                else if (opc >= 0x28 && sys->jit->codeMap[paddr0 >> 6]) sys->jit->opHist[195]++;
                else sys->jit->opHist[196]++;
            }
        }
    }
    if (haveOp) h64_cpu_step_op(sys, op);
    else h64_cpu_step(sys);
    if (sys->jit->opHist)
    {
        u32 i;
        op = cpu->lastOp;
        i = op >> 26;
        if (i == 0) i = 64 + (op & 63);
        else if (i == 0x11 && ((op >> 21) & 31) < 0x10) i = 200 + ((op >> 21) & 31);   // MFC1 MTC1 CFC1 CTC1 BC1...
        else if (i == 0x11 && ((op >> 21) & 31) >= 0x14) i = 216 + (op & 1);              // CVT.S/D from W or L
        else if (i == 0x11) i = 128 + (op & 63);
        sys->jit->opHist[i]++;
    }
    if (cpu->exceptionRaised || cpu->pc != sext32(expectedNext)) return 1;
    return h64_jit_should_exit(sys);
}

static u64 fn_addr(int (*f)(H64System *, u32, u32, u32)) { return (u64)(uintptr_t)f; }

// ---- Code generation context ----
struct Gen
{
    H64System *sys;
    H64PpcCode c;
    u32 pc0;          // block start
    u32 paddr0;       // its physical address
    u32 pending;      // native instructions not yet added to cycles/instructions
    u32 exits[H64_JIT_MAX_INSNS * 4];
    u32 nExits;
    int native;       // native code allowed (kernel-mode block)
    int fpu;          // native FPU arithmetic allowed (host FPU flags readable)
    u32 cpi;          // cycles per instruction (sys->cpu.cpi)
    u32 bc1Slow;      // BC1 with COP1 unusable: the branch to the interpreted fallback
    int hasBc1Slow;
};

static void add_exit(Gen *g, u32 at) { if (g->nExits < H64_JIT_MAX_INSNS * 4) g->exits[g->nExits++] = at; }

static void load_gpr(Gen *g, u32 reg, u32 mips)
{
    if (mips == 0) ppc_li(&g->c, reg, 0);
    else ppc_ld(&g->c, reg, OFF_GPR(mips), JR_CPU);
}

static void store_gpr(Gen *g, u32 reg, u32 mips)
{
    if (mips != 0) ppc_std(&g->c, reg, OFF_GPR(mips), JR_CPU);
}

static void add_to(Gen *g, s32 off, s32 n)
{
    if (!n) return;
    ppc_ld(&g->c, 3, off, JR_CPU);
    ppc_addi(&g->c, 3, 3, n);
    ppc_std(&g->c, 3, off, JR_CPU);
}

// cycles/instructions += pending; pc = next; nextPc = next + 4; branchPending = 0.
static void sync_to(Gen *g, u32 pending, u32 next)
{
    add_to(g, OFF_CYCLES, (s32)(pending * g->cpi));
    add_to(g, OFF_INSNS, (s32)pending);
    ppc_li64(&g->c, 3, sext32(next));
    ppc_std(&g->c, 3, OFF_PC, JR_CPU);
    ppc_addi(&g->c, 3, 3, 4);
    ppc_std(&g->c, 3, OFF_NEXTPC, JR_CPU);
    ppc_li(&g->c, 3, 0);
    ppc_stw(&g->c, 3, OFF_BRANCH, JR_CPU);
}

static void exit_block(Gen *g) { add_exit(g, ppc_b_fwd(&g->c)); }

// ---- Block linking ----
// An exit whose next pc is fixed (state already synced). If no event falls
// within the next H64_JIT_MAX_INSNS instructions and h64_jit_run's end is not
// reached, it goes on into the next block: at first through the dispatcher,
// which then patches the branch below into a direct branch to that block's
// body (all blocks share the same prologue, registers and frame). The
// dispatcher's per-block state is kept conservative: blockEndCycles covers
// any block, and any invalidation stops the running block.
static void load_ptr(Gen *g, u32 rt, s32 off, u32 ra)
{
#if defined(H64_JIT_ABI_XBOX)
    ppc_lwz(&g->c, rt, off, ra);
#else
    ppc_ld(&g->c, rt, off, ra);
#endif
}

static void link_exit(Gen *g, u32 targetPc)
{
    H64PpcCode *c = &g->c;
    u32 patchAt;
    if (!g->native || g->sys->jit->noLink)
    {
        exit_block(g);
        return;
    }
    ppc_ld(c, 3, OFF_CYCLES, JR_CPU);
    ppc_addi(c, 3, 3, (s32)(H64_JIT_MAX_INSNS * g->cpi));
    ppc_ld(c, 4, (s32)(offsetof(H64System, sched) + offsetof(H64Scheduler, next)), JR_SYS);
    ppc_cmpld(c, 0, 3, 4);
    add_exit(g, ppc_bc_fwd(c, 12, 0, PPC_GT));     // an event comes first
    load_ptr(g, 5, (s32)offsetof(H64System, jit), JR_SYS);
    ppc_ld(c, 4, (s32)offsetof(H64Jit, runEnd), 5);
    ppc_cmpld(c, 0, 3, 4);
    add_exit(g, ppc_bc_fwd(c, 12, 0, PPC_GT));     // the end of the run
    ppc_std(c, 3, (s32)offsetof(H64Jit, blockEndCycles), 5);
    patchAt = c->pos;
    ppc_put(c, 0x48000004u);                        // b +4; linked: b <next block's body>
    // Not linked yet: tell the dispatcher where this exit is and where it goes.
#if defined(H64_JIT_ABI_XBOX)
    ppc_li32u(c, 3, (u32)(uintptr_t)(c->buf + patchAt));
    ppc_stw(c, 3, (s32)offsetof(H64Jit, lastExit), 5);
#else
    ppc_li64(c, 3, (u64)(uintptr_t)(c->buf + patchAt));
    ppc_std(c, 3, (s32)offsetof(H64Jit, lastExit), 5);
#endif
    ppc_li32u(c, 3, targetPc);
    ppc_stw(c, 3, (s32)offsetof(H64Jit, lastExitTarget), 5);
    exit_block(g);
}

// The exit after a native branch: linked per outcome when the target is fixed.
static void branch_exit(Gen *g, int dynamicTarget, u64 target, u32 fallthrough)
{
    u32 notTaken;
    if (dynamicTarget || !g->native)
    {
        exit_block(g);
        return;
    }
    ppc_cmpdi(&g->c, 0, R_COND, 0);
    notTaken = ppc_bc_fwd(&g->c, 12, 0, PPC_EQ);
    link_exit(g, (u32)target);
    ppc_patch_here(&g->c, notTaken);
    link_exit(g, fallthrough);
}

// Calls the interpreter for the instruction at cpu->pc with the state as it is
// (a delay slot), ignoring its answer: the block ends right after.
static void call_interp_raw(Gen *g)
{
    ppc_mr(&g->c, 3, JR_SYS);
    ppc_li(&g->c, 4, 0);
    ppc_li(&g->c, 6, 0);   // the interpreter fetches the instruction
#if defined(H64_JIT_ABI_ELFV1)
    ppc_ld(&g->c, 0, 0, JR_TMP);
    ppc_ld(&g->c, 2, 8, JR_TMP);
    ppc_mtctr(&g->c, 0);
#else
    ppc_mtctr(&g->c, JR_TMP);
#endif
    ppc_bctrl(&g->c);
}

// The interpreter runs instruction `pc` (state synced); stop on its request.
static void call_interp(Gen *g, u32 pc)
{
    u32 op = h64_load_be32(g->sys->rdram + g->paddr0 + (pc - g->pc0));
    sync_to(g, g->pending, pc);
    g->pending = 0;
    ppc_mr(&g->c, 3, JR_SYS);
    ppc_li32u(&g->c, 4, pc + 4);
    ppc_li32u(&g->c, 5, op);   // the instruction, as read when compiling
    ppc_li(&g->c, 6, 1);
#if defined(H64_JIT_ABI_ELFV1)
    ppc_ld(&g->c, 0, 0, JR_TMP);
    ppc_ld(&g->c, 2, 8, JR_TMP);
    ppc_mtctr(&g->c, 0);
#else
    ppc_mtctr(&g->c, JR_TMP);
#endif
    ppc_bctrl(&g->c);
    ppc_cmpwi(&g->c, 0, 3, 0);
    add_exit(g, ppc_bc_fwd(&g->c, 4, 0, PPC_EQ));   // bne exit
}

// ---- Prologue / epilogue ----
static void emit_prologue(Gen *g)
{
    H64PpcCode *c = &g->c;
    u32 r;
#if defined(H64_JIT_ABI_XBOX)
    ppc_stwu(c, 1, -FRAME, 1);   // 32-bit back chain, as the XDK compiler does
#else
    ppc_stdu(c, 1, -FRAME, 1);
#endif
    ppc_mflr(c, 0);
    ppc_std(c, 0, FRAME - 8, 1);
    for (r = 24; r < 32; r++) ppc_std(c, r, FRAME - 16 - 8 * (s32)(31 - r), 1);
    ppc_mr(c, JR_SYS, 3);
    ppc_addi(c, JR_CPU, JR_SYS, (s32)offsetof(H64System, cpu));
#if defined(H64_JIT_ABI_XBOX)
    ppc_lwz(c, JR_RDRAM, (s32)offsetof(H64System, rdram), JR_SYS);
#else
    ppc_ld(c, JR_RDRAM, (s32)offsetof(H64System, rdram), JR_SYS);
#endif
#if defined(H64_JIT_ABI_XBOX)
    ppc_li32u(c, JR_TMP, (u32)fn_addr(helper_interp));
#else
    ppc_li64(c, JR_TMP, fn_addr(helper_interp));
#endif
}

static void emit_epilogue(Gen *g)
{
    H64PpcCode *c = &g->c;
    u32 r;
    ppc_ld(c, 0, FRAME - 8, 1);
    ppc_mtlr(c, 0);
    for (r = 24; r < 32; r++) ppc_ld(c, r, FRAME - 16 - 8 * (s32)(31 - r), 1);
    ppc_addi(c, 1, 1, FRAME);
    ppc_blr(c);
}

// ---- Instruction classes ----
static int is_branch(u32 op)
{
    u32 opc = op >> 26, rt = (op >> 16) & 31, rs = (op >> 21) & 31, funct = op & 63;
    if (opc == 0) return funct == 0x08 || funct == 0x09;                   // JR, JALR
    if (opc == 1) return (rt & 0x0C) == 0 && (rt & 0x10 ? rt <= 0x13 : 1);   // BLTZ.. / BLTZAL..
    if (opc >= 2 && opc <= 7) return 1;                                     // J JAL BEQ BNE BLEZ BGTZ
    if (opc >= 0x14 && opc <= 0x17) return 1;                               // BEQL..BGTZL
    if (opc == 0x11 && rs == 0x08) return 1;                                // BC1x
    return 0;
}

// COP0 (MTC0, ERET, TLB...), except MFC0/DMFC0, which change nothing.
static int ends_block(u32 op) { return (op >> 26) == 0x10 && ((op >> 21) & 31) != 0 && ((op >> 21) & 31) != 1; }

// ---- Native ALU ----
static void slt_result(Gen *g, int unsignedCmp, u32 rd)
{
    // r3 < r4 ? 1 : 0
    u32 at;
    if (unsignedCmp) ppc_cmpld(&g->c, 0, 3, 4);
    else ppc_cmpd(&g->c, 0, 3, 4);
    ppc_li(&g->c, 5, 1);
    at = ppc_bc_fwd(&g->c, 12, 0, PPC_LT);
    ppc_li(&g->c, 5, 0);
    ppc_patch_here(&g->c, at);
    store_gpr(g, 5, rd);
}

// Returns 1 if `op` was emitted natively (no exception possible).
static int emit_alu(Gen *g, u32 op)
{
    H64PpcCode *c = &g->c;
    u32 opc = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    s32 simm = (s32)(s16)(op & 0xFFFF);
    u32 uimm = op & 0xFFFF;
    if (opc == 0)
    {
        switch (op & 63)
        {
        case 0x00: case 0x02: case 0x03:   // SLL SRL SRA
            if (rd == 0) return 1;
            load_gpr(g, 3, rt);
            if ((op & 63) == 0x00) ppc_rlwinm(c, 3, 3, sa, 0, 31 - sa);
            else if ((op & 63) == 0x02) { if (sa) ppc_rlwinm(c, 3, 3, 32 - sa, sa, 31); }
            else ppc_sradi(c, 3, 3, sa);
            ppc_extsw(c, 3, 3);
            store_gpr(g, 3, rd);
            return 1;
        case 0x04: case 0x06: case 0x07:   // SLLV SRLV SRAV
            if (rd == 0) return 1;
            load_gpr(g, 3, rt);
            load_gpr(g, 4, rs);
            ppc_rlwinm(c, 4, 4, 0, 27, 31);   // & 31
            if ((op & 63) == 0x04) ppc_slw(c, 3, 3, 4);
            else if ((op & 63) == 0x06) { ppc_clrldi(c, 3, 3, 32); ppc_srw(c, 3, 3, 4); }
            else ppc_srad(c, 3, 3, 4);
            ppc_extsw(c, 3, 3);
            store_gpr(g, 3, rd);
            return 1;
        case 0x14: case 0x16: case 0x17:   // DSLLV DSRLV DSRAV
            if (rd == 0) return 1;
            load_gpr(g, 3, rt);
            load_gpr(g, 4, rs);
            ppc_rlwinm(c, 4, 4, 0, 26, 31);   // & 63
            if ((op & 63) == 0x14) ppc_sld(c, 3, 3, 4);
            else if ((op & 63) == 0x16) ppc_srd(c, 3, 3, 4);
            else ppc_srad(c, 3, 3, 4);
            store_gpr(g, 3, rd);
            return 1;
        case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:   // DSLL DSRL DSRA (+32)
        {
            u32 n = sa + (((op & 63) >= 0x3C) ? 32 : 0), f = (op & 63) & 0x3B;
            if (rd == 0) return 1;
            load_gpr(g, 3, rt);
            if (n)
            {
                if (f == 0x38) ppc_sldi(c, 3, 3, n);
                else if (f == 0x3A) ppc_srdi(c, 3, 3, n);
                else ppc_sradi(c, 3, 3, n);
            }
            store_gpr(g, 3, rd);
            return 1;
        }
        case 0x10: case 0x12:   // MFHI MFLO
            if (rd == 0) return 1;
            ppc_ld(c, 3, (op & 63) == 0x10 ? OFF_HI : OFF_LO, JR_CPU);
            store_gpr(g, 3, rd);
            return 1;
        case 0x11: case 0x13:   // MTHI MTLO
            load_gpr(g, 3, rs);
            ppc_std(c, 3, (op & 63) == 0x11 ? OFF_HI : OFF_LO, JR_CPU);
            return 1;
        case 0x0F: return 1;    // SYNC
        case 0x18: case 0x19:   // MULT MULTU: 32x32 -> 64, lo/hi sign-extended halves
            load_gpr(g, 3, rs);
            load_gpr(g, 4, rt);
            if ((op & 63) == 0x18) { ppc_extsw(c, 3, 3); ppc_extsw(c, 4, 4); }
            else { ppc_clrldi(c, 3, 3, 32); ppc_clrldi(c, 4, 4, 32); }
            ppc_mulld(c, 5, 3, 4);
            ppc_extsw(c, 3, 5);
            ppc_std(c, 3, OFF_LO, JR_CPU);
            ppc_srdi(c, 3, 5, 32);
            ppc_extsw(c, 3, 3);
            ppc_std(c, 3, OFF_HI, JR_CPU);
            return 1;
        case 0x1A: case 0x1B:   // DIV DIVU, with the interpreter's results for /0 and INT_MIN/-1
        {
            u32 zero, ovf = 0, done1, done2, done3 = 0, normal = 0;
            int sgn = (op & 63) == 0x1A;
            load_gpr(g, 3, rs);
            load_gpr(g, 4, rt);
            if (sgn) { ppc_extsw(c, 3, 3); ppc_extsw(c, 4, 4); }
            else { ppc_clrldi(c, 3, 3, 32); ppc_clrldi(c, 4, 4, 32); }
            ppc_cmpdi(c, 0, 4, 0);
            zero = ppc_bc_fwd(c, 12, 0, PPC_EQ);
            if (sgn)
            {
                ppc_cmpdi(c, 1, 4, -1);
                normal = ppc_bc_fwd(c, 4, 1, PPC_EQ);
                ppc_lis(c, 5, -32768);              // 0xFFFFFFFF80000000
                ppc_cmpd(c, 0, 3, 5);
                ovf = ppc_bc_fwd(c, 12, 0, PPC_EQ);
                ppc_patch_here(c, normal);
            }
            if (sgn) ppc_divw(c, 5, 3, 4);
            else ppc_divwu(c, 5, 3, 4);
            ppc_mullw(c, 6, 5, 4);
            ppc_subf(c, 6, 6, 3);
            ppc_extsw(c, 5, 5);
            ppc_extsw(c, 6, 6);
            done1 = ppc_b_fwd(c);
            ppc_patch_here(c, zero);                // lo = a < 0 ? 1 : -1 (DIVU: -1), hi = a
            ppc_li(c, 5, -1);
            if (sgn)
            {
                u32 pos;
                ppc_cmpdi(c, 0, 3, 0);
                pos = ppc_bc_fwd(c, 4, 0, PPC_LT);
                ppc_li(c, 5, 1);
                ppc_patch_here(c, pos);
            }
            ppc_extsw(c, 6, 3);
            done2 = ppc_b_fwd(c);
            if (sgn)
            {
                ppc_patch_here(c, ovf);             // INT_MIN / -1: lo = a, hi = 0
                ppc_mr(c, 5, 3);
                ppc_li(c, 6, 0);
                done3 = ppc_b_fwd(c);
            }
            ppc_patch_here(c, done1);
            ppc_patch_here(c, done2);
            if (sgn) ppc_patch_here(c, done3);
            ppc_std(c, 5, OFF_LO, JR_CPU);
            ppc_std(c, 6, OFF_HI, JR_CPU);
            return 1;
        }
        case 0x1C: case 0x1D:   // DMULT DMULTU: 64x64 -> 128
            load_gpr(g, 3, rs);
            load_gpr(g, 4, rt);
            ppc_mulld(c, 5, 3, 4);
            if ((op & 63) == 0x1C) ppc_mulhd(c, 6, 3, 4);
            else ppc_mulhdu(c, 6, 3, 4);
            ppc_std(c, 5, OFF_LO, JR_CPU);
            ppc_std(c, 6, OFF_HI, JR_CPU);
            return 1;
        case 0x21: case 0x23: case 0x2D: case 0x2F:   // ADDU SUBU DADDU DSUBU
            if (rd == 0) return 1;
            load_gpr(g, 3, rs);
            load_gpr(g, 4, rt);
            if ((op & 63) == 0x21 || (op & 63) == 0x2D) ppc_add(c, 3, 3, 4);
            else ppc_subf(c, 3, 4, 3);
            if ((op & 63) <= 0x23) ppc_extsw(c, 3, 3);
            store_gpr(g, 3, rd);
            return 1;
        case 0x24: case 0x25: case 0x26: case 0x27:   // AND OR XOR NOR
            if (rd == 0) return 1;
            load_gpr(g, 3, rs);
            load_gpr(g, 4, rt);
            if ((op & 63) == 0x24) ppc_and(c, 3, 3, 4);
            else if ((op & 63) == 0x25) ppc_or(c, 3, 3, 4);
            else if ((op & 63) == 0x26) ppc_xor(c, 3, 3, 4);
            else ppc_nor(c, 3, 3, 4);
            store_gpr(g, 3, rd);
            return 1;
        case 0x2A: case 0x2B:   // SLT SLTU
            if (rd == 0) return 1;
            load_gpr(g, 3, rs);
            load_gpr(g, 4, rt);
            slt_result(g, (op & 63) == 0x2B, rd);
            return 1;
        }
        return 0;
    }
    switch (opc)
    {
    case 0x09: case 0x19:   // ADDIU DADDIU
        if (rt == 0) return 1;
        load_gpr(g, 3, rs);
        ppc_addi(c, 3, 3, simm);
        if (opc == 0x09) ppc_extsw(c, 3, 3);
        store_gpr(g, 3, rt);
        return 1;
    case 0x0A: case 0x0B:   // SLTI SLTIU (the immediate is sign-extended for both)
        if (rt == 0) return 1;
        load_gpr(g, 3, rs);
        ppc_li(c, 4, simm);
        slt_result(g, opc == 0x0B, rt);
        return 1;
    case 0x0C: case 0x0D: case 0x0E:   // ANDI ORI XORI
        if (rt == 0) return 1;
        load_gpr(g, 3, rs);
        if (opc == 0x0C) ppc_andi_(c, 3, 3, uimm);
        else if (opc == 0x0D) ppc_ori(c, 3, 3, uimm);
        else ppc_xori(c, 3, 3, uimm);
        store_gpr(g, 3, rt);
        return 1;
    case 0x0F:   // LUI
        if (rt == 0) return 1;
        ppc_lis(c, 3, simm);
        store_gpr(g, 3, rt);
        return 1;
    }
    return 0;
}

// ---- Native loads and stores (RDRAM through KSEG0/KSEG1) ----
// Fast path: a sign-extended 32-bit address in 0x80000000-0xBFFFFFFF, aligned,
// within RDRAM; stores also need no code on the page and no MI repeat mode.
// Anything else runs the instruction in the interpreter.
//
// FPU register access for LWC1/LDC1/SWC1/SDC1 (h64_fpr_get/set32/64): with
// FR = 1 register ft is fgr[ft]; with FR = 0 an odd ft is the high word of
// fgr[ft - 1] (32-bit) or fgr[ft - 1] itself (64-bit). For an even ft both
// modes are the same. r7 holds Status & FR (from the guard), r5 the value.
static void fpu_access(Gen *g, u32 ft, u32 size, int store)
{
    H64PpcCode *c = &g->c;
    u32 alt = 0, done = 0, pass, n = (ft & 1) ? 2 : 1;
    for (pass = 0; pass < n; pass++)
    {
        // pass 0: FR = 1 (or an even register); pass 1: FR = 0 with an odd register.
        s32 off;
        if (pass == 0 && n == 2)
        {
            ppc_cmpdi(c, 0, 7, 0);
            alt = ppc_bc_fwd(c, 12, 0, PPC_EQ);   // beq: FR = 0
        }
        if (pass == 1) ppc_patch_here(c, alt);
        if (size == 4) off = pass == 0 ? OFF_FGR(ft) + 4 : OFF_FGR(ft - 1);
        else off = pass == 0 ? OFF_FGR(ft) : OFF_FGR(ft - 1);
        if (store)
        {
            if (size == 4) ppc_lwz(c, 5, off, JR_CPU);
            else ppc_ld(c, 5, off, JR_CPU);
        }
        else
        {
            if (size == 4) ppc_stw(c, 5, off, JR_CPU);
            else ppc_std(c, 5, off, JR_CPU);
        }
        if (pass == 0 && n == 2) done = ppc_b_fwd(c);
    }
    if (n == 2) ppc_patch_here(c, done);
}

// emit_mem_fast emits the fast path only and appends the branches to the
// slow path to slow[]; it returns 0 when op is not a native memory access.
static int emit_mem_fast(Gen *g, u32 op, u32 *slow, u32 *nSlowOut)
{
    H64PpcCode *c = &g->c;
    u32 opc = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, size, store = 0, nSlow = *nSlowOut, fpu = 0;
    s32 simm = (s32)(s16)(op & 0xFFFF);
    switch (opc)
    {
    case 0x31: size = 4; fpu = 1; break;               // LWC1
    case 0x35: size = 8; fpu = 1; break;               // LDC1
    case 0x39: size = 4; store = 1; fpu = 1; break;    // SWC1
    case 0x3D: size = 8; store = 1; fpu = 1; break;    // SDC1
    case 0x20: case 0x24: size = 1; break;    // LB LBU
    case 0x21: case 0x25: size = 2; break;    // LH LHU
    case 0x23: case 0x27: size = 4; break;    // LW LWU
    case 0x37: size = 8; break;               // LD
    case 0x28: size = 1; store = 1; break;    // SB
    case 0x29: size = 2; store = 1; break;    // SH
    case 0x2B: size = 4; store = 1; break;    // SW
    case 0x3F: size = 8; store = 1; break;    // SD
    default: return 0;
    }
    if (fpu)
    {
        // COP1 must be usable (else the slow path raises the exception). r7
        // keeps Status.FR for the register selection below.
        ppc_ld(c, 5, OFF_COP0(CP0_STATUS), JR_CPU);
        ppc_andis_(c, 7, 5, 0x0400);     // FR
        ppc_andis_(c, 5, 5, 0x2000);     // CU1
        slow[nSlow++] = ppc_bc_fwd(c, 12, 0, PPC_EQ);   // beq: CU1 clear
    }
    load_gpr(g, 3, rs);
    ppc_addi(c, 3, 3, simm);
    ppc_extsw(c, 4, 3);                       // a sign-extended 32-bit address?
    ppc_cmpd(c, 0, 4, 3);
    slow[nSlow++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    ppc_rlwinm(c, 4, 3, 2, 30, 31);           // bits 31..30 == 10: KSEG0 or KSEG1
    ppc_cmplwi(c, 0, 4, 2);
    slow[nSlow++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    if (size > 1)
    {
        ppc_andi_(c, 4, 3, size - 1);
        slow[nSlow++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    }
    ppc_rlwinm(c, 4, 3, 0, 3, 31);            // physical address (& 0x1FFFFFFF)
    ppc_rlwinm(c, 5, 4, 9, 23, 31);           // >> 23: within the 8 MB of RDRAM?
    ppc_cmplwi(c, 0, 5, 0);
    slow[nSlow++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    if (store)
    {
        H64Jit *j = g->sys->jit;
        // No block over this 64-byte chunk (codeMap[paddr >> 6] == 0)...
        ppc_li64(c, 6, (u64)(uintptr_t)j->codeMap);
        ppc_rlwinm(c, 5, 4, 32 - 5, 5, 30);     // (paddr >> 6) * 2
        ppc_lhzx(c, 5, 6, 5);
        ppc_cmpdi(c, 0, 5, 0);
        slow[nSlow++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        // ...and no MI repeat mode.
        ppc_li64(c, 6, (u64)(uintptr_t)&g->sys->mi.mode);
        ppc_lwz(c, 5, 0, 6);
        ppc_andi_(c, 5, 5, 0x80);
        slow[nSlow++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        if (fpu) fpu_access(g, rt, size, 1);   // the value to store, in r5
        else load_gpr(g, 5, rt);
        switch (size)
        {
        case 1: ppc_stbx(c, 5, JR_RDRAM, 4); break;
        case 2: ppc_sthx(c, 5, JR_RDRAM, 4); break;
        case 4: ppc_stwx(c, 5, JR_RDRAM, 4); break;
        default: ppc_stdx(c, 5, JR_RDRAM, 4); break;
        }
    }
    else if (fpu)
    {
        // LWC1 replaces one word of a 64-bit register (big-endian host: the
        // low word is at +4), LDC1 a whole register.
        if (size == 4) ppc_lwzx(c, 5, JR_RDRAM, 4);
        else ppc_ldx(c, 5, JR_RDRAM, 4);
        fpu_access(g, rt, size, 0);
    }
    else
    {
        switch (opc)
        {
        case 0x20: ppc_lbzx(c, 5, JR_RDRAM, 4); ppc_extsb(c, 5, 5); break;
        case 0x24: ppc_lbzx(c, 5, JR_RDRAM, 4); break;
        case 0x21: ppc_lhax(c, 5, JR_RDRAM, 4); break;
        case 0x25: ppc_lhzx(c, 5, JR_RDRAM, 4); break;
        case 0x23: ppc_lwax(c, 5, JR_RDRAM, 4); break;
        case 0x27: ppc_lwzx(c, 5, JR_RDRAM, 4); break;
        default: ppc_ldx(c, 5, JR_RDRAM, 4); break;
        }
        store_gpr(g, 5, rt);
    }
    *nSlowOut = nSlow;
    return 1;
}

// Slow path shared by the native instructions that can leave their fast path:
// the interpreter runs the instruction, then the block goes on with the
// counters as the fast path leaves them (this instruction pending).
static void emit_slow_tail(Gen *g, u32 pc, const u32 *slow, u32 nSlow, u32 done)
{
    H64PpcCode *c = &g->c;
    u32 i;
    for (i = 0; i < nSlow; i++) ppc_patch_here(c, slow[i]);
    {
        u32 pend = g->pending;
        call_interp(g, pc);
        add_to(g, OFF_CYCLES, -(s32)((pend + 1) * g->cpi));
        add_to(g, OFF_INSNS, -(s32)(pend + 1));
        g->pending = pend;
    }
    ppc_patch_here(c, done);
    g->pending++;
}

static int emit_mem(Gen *g, u32 op, u32 pc)
{
    u32 slow[8], nSlow = 0, done;
    if (!emit_mem_fast(g, op, slow, &nSlow)) return 0;
    done = ppc_b_fwd(&g->c);
    emit_slow_tail(g, pc, slow, nSlow, done);
    return 1;
}

// ADDI, ADD and SUB: native, with the 32-bit overflow checked; on overflow the
// interpreter runs the instruction and raises the exception.
static int emit_ovf_fast(Gen *g, u32 op, u32 *slow, u32 *nSlow)
{
    H64PpcCode *c = &g->c;
    u32 opc = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, dst;
    if (opc == 0x08)
    {
        dst = rt;
        load_gpr(g, 3, rs);
        ppc_extsw(c, 3, 3);
        ppc_li(c, 4, (s32)(s16)(op & 0xFFFF));
        ppc_add(c, 5, 3, 4);
    }
    else if (opc == 0 && ((op & 63) == 0x20 || (op & 63) == 0x22))
    {
        dst = rd;
        load_gpr(g, 3, rs);
        load_gpr(g, 4, rt);
        ppc_extsw(c, 3, 3);
        ppc_extsw(c, 4, 4);
        if ((op & 63) == 0x20) ppc_add(c, 5, 3, 4);
        else ppc_subf(c, 5, 4, 3);
    }
    else
        return 0;
    ppc_extsw(c, 6, 5);
    ppc_cmpd(c, 0, 6, 5);
    slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);   // the 64-bit sum is not a 32-bit value: overflow
    store_gpr(g, 6, dst);
    return 1;
}

static int emit_ovf(Gen *g, u32 op, u32 pc)
{
    u32 slow[1], nSlow = 0, done;
    if (!emit_ovf_fast(g, op, slow, &nSlow)) return 0;
    done = ppc_b_fwd(&g->c);
    emit_slow_tail(g, pc, slow, nSlow, done);
    return 1;
}

// ---- Native FPU ----
// COP1 arithmetic (ADD SUB MUL DIV), compares, MOV, CVT.D.S, CVT.S.D, TRUNC.W
// and CVT.W in .S and .D run on the host FPU when the result is certain to be
// the interpreter's: operands normal or zero, guest rounding mode nearest
// (the host's), and no host flag but inexact (FPSCR VX, OX, UX, ZX clear;
// FI, the non-sticky inexact bit of the last operation, gives the MIPS
// Inexact cause) and a normal, zero or infinite result. Anything else (NaN,
// denormal, overflow, an enabled Inexact trap...) goes to the slow path,
// which is the interpreter, before anything is written. Host FPU flags must
// be readable (not in Xenia: h64_fenv_reliable).
// f4 = smallest normal number of the format, f5 = 0.
static void fp_check(Gen *g, u32 f, u32 *slow, u32 *nSlow)
{
    H64PpcCode *c = &g->c;
    u32 ok;
    ppc_fabs(c, 3, f);
    ppc_fcmpu(c, 1, 3, 4);
    slow[(*nSlow)++] = ppc_bc_fwd(c, 12, 1, PPC_UN);   // NaN
    ok = ppc_bc_fwd(c, 4, 1, PPC_LT);                  // |x| >= min normal (or infinite)
    ppc_fcmpu(c, 1, 3, 5);
    slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 1, PPC_EQ);    // denormal
    ppc_patch_here(c, ok);
}

// FPSCR after the operation: VX/OX/UX/ZX to the slow path, r7 = FI.
static void fp_flags(Gen *g, u32 *slow, u32 *nSlow)
{
    H64PpcCode *c = &g->c;
    ppc_mffs(c, 0);
    ppc_stfd(c, 0, OFF_JITSCRATCH, JR_CPU);
    ppc_lwz(c, 6, OFF_JITSCRATCH + 4, JR_CPU);
    ppc_andis_(c, 7, 6, 0x3C00);
    slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    ppc_rlwinm(c, 7, 6, 15, 31, 31);
}

// FCR31 (in r5) with cause = Inexact if r7 (and the sticky flag), else no
// cause; an enabled Inexact trap goes to the slow path.
static void fp_inexact(Gen *g, u32 *slow, u32 *nSlow)
{
    H64PpcCode *c = &g->c;
    ppc_rlwinm(c, 8, 5, 25, 31, 31);   // Enable I
    ppc_and_(c, 8, 8, 7);
    slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    ppc_rlwinm(c, 5, 5, 0, 20, 13);    // clear the cause field
    ppc_rlwinm(c, 8, 7, 12, 0, 19);
    ppc_or(c, 5, 5, 8);
    ppc_rlwinm(c, 8, 7, 2, 0, 29);
    ppc_or(c, 5, 5, 8);
    ppc_stw(c, 5, OFF_FCR31, JR_CPU);
}

static void fp_store32_zext(Gen *g, u32 fd)
{
    ppc_li(&g->c, 8, 0);
    ppc_stw(&g->c, 8, OFF_FGR(fd), JR_CPU);
}

static int emit_fpu_fast(Gen *g, u32 op, u32 *slow, u32 *nSlow)
{
    H64PpcCode *c = &g->c;
    u32 fmt = (op >> 21) & 31, ft = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31, funct = op & 63;
    int dbl = fmt == 0x11, arith = funct <= 3, cmp = funct >= 0x30;
    int cvtd = !dbl && funct == 0x21, cvts = dbl && funct == 0x20, toint = funct == 0x0D || funct == 0x24;
    int fromW = fmt == 0x14 && (funct == 0x20 || funct == 0x21);
    // Moves (MFC1 DMFC1 CFC1 MTC1 DMTC1 CTC1) need no host flags.
    int move = fmt == 0 || fmt == 1 || fmt == 2 || fmt == 4 || fmt == 5 || fmt == 6;
    s32 offS, offT;
    if ((op >> 26) != 0x11) return 0;
    if (!move)
    {
        if (!g->fpu || (fmt != 0x10 && fmt != 0x11 && !fromW)) return 0;
        if (!arith && !cmp && funct != 0x06 && funct != 0x05 && funct != 0x07 && !cvtd && !cvts && !toint && !fromW) return 0;
    }

    // COP1 usable; an odd fs register needs FR = 1 (FR = 0 would use fs - 1).
    ppc_ld(c, 5, OFF_COP0(CP0_STATUS), JR_CPU);
    ppc_andis_(c, 6, 5, 0x2000);
    slow[(*nSlow)++] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
    if ((fs & 1) && fmt != 2 && fmt != 6 && !move)
    {
        ppc_andis_(c, 6, 5, 0x0400);
        slow[(*nSlow)++] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
    }
    if (move)
    {
        // With FR = 0 an odd register is the high word of the even one (32-bit
        // moves) or the even register itself (64-bit moves).
        u32 pass, n = ((fs & 1) && fmt != 2 && fmt != 6) ? 2 : 1, alt = 0, done = 0;
        if (fmt == 0 || fmt == 1 || fmt == 4 || fmt == 5)
        {
            for (pass = 0; pass < n; pass++)
            {
                s32 off32 = pass ? OFF_FGR(fs - 1) : OFF_FGR(fs) + 4, off64 = pass ? OFF_FGR(fs - 1) : OFF_FGR(fs);
                if (pass == 0 && n == 2)
                {
                    ppc_andis_(c, 6, 5, 0x0400);   // FR
                    alt = ppc_bc_fwd(c, 12, 0, PPC_EQ);
                }
                if (pass == 1) ppc_patch_here(c, alt);
                switch (fmt)
                {
                case 0: ppc_lwa(c, 3, off32, JR_CPU); store_gpr(g, 3, ft); break;    // MFC1
                case 1: ppc_ld(c, 3, off64, JR_CPU); store_gpr(g, 3, ft); break;     // DMFC1
                case 4: load_gpr(g, 3, ft); ppc_stw(c, 3, off32, JR_CPU); break;     // MTC1
                default: load_gpr(g, 3, ft); ppc_std(c, 3, off64, JR_CPU); break;    // DMTC1
                }
                if (pass == 0 && n == 2) done = ppc_b_fwd(c);
            }
            if (n == 2) ppc_patch_here(c, done);
            return 1;
        }
        switch (fmt)
        {
        case 2:                                                                        // CFC1
            if (fs == 31) ppc_lwa(c, 5, OFF_FCR31, JR_CPU);
            else ppc_li(c, 5, fs == 0 ? 0x0A00 : 0);
            store_gpr(g, 5, ft);
            break;
        default:                                                                       // CTC1
            if (fs != 31) break;
            load_gpr(g, 5, ft);
            ppc_lis(c, 6, 0x0183);
            ppc_ori(c, 6, 6, 0xFFFF);
            ppc_and(c, 5, 5, 6);              // FCR31_WRITABLE
            // A cause written with its enable, or cause E, traps: the slow path.
            ppc_rlwinm(c, 6, 5, 20, 27, 31);
            ppc_rlwinm(c, 7, 5, 25, 27, 31);
            ppc_and_(c, 6, 6, 7);
            slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_andis_(c, 6, 5, 0x0002);
            slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
            ppc_stw(c, 5, OFF_FCR31, JR_CPU);
            break;
        }
        return 1;
    }
    if (funct == 0x06)   // MOV: the whole register
    {
        ppc_ld(c, 5, OFF_FGR(fs), JR_CPU);
        ppc_std(c, 5, OFF_FGR(fd), JR_CPU);
        return 1;
    }

    ppc_lwz(c, 5, OFF_FCR31, JR_CPU);
    if (fromW)
    {
        // CVT.S.W, CVT.D.W: the integer through memory (fcfid converts a
        // doubleword), exact as a double; rounded once to single.
        ppc_lwa(c, 6, OFF_FGR(fs) + 4, JR_CPU);
        ppc_std(c, 6, OFF_JITSCRATCH, JR_CPU);
        ppc_lfd(c, 1, OFF_JITSCRATCH, JR_CPU);
        ppc_fcfid(c, 1, 1);
        if (funct == 0x21)
        {
            ppc_rlwinm(c, 5, 5, 0, 20, 13);
            ppc_stw(c, 5, OFF_FCR31, JR_CPU);
            ppc_stfd(c, 1, OFF_FGR(fd), JR_CPU);
            return 1;
        }
        ppc_andi_(c, 6, 5, 3);   // rounding mode nearest
        slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        ppc_frsp(c, 1, 1);
        fp_flags(g, slow, nSlow);
        fp_inexact(g, slow, nSlow);
        fp_store32_zext(g, fd);
        ppc_stfs(c, 1, OFF_FGR(fd) + 4, JR_CPU);
        return 1;
    }
    if (arith || cvts)
    {
        ppc_andi_(c, 6, 5, 3);   // rounding mode nearest
        slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    }
    if (funct == 0x24)
    {
        // CVT.W: nearest or towards zero (IDO's (int) casts set RM = 1 around it); cr0.eq = nearest.
        ppc_andi_(c, 6, 5, 2);
        slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
        ppc_andi_(c, 6, 5, 1);
    }
    offS = dbl ? OFF_FGR(fs) : OFF_FGR(fs) + 4;
    offT = dbl ? OFF_FGR(ft) : OFF_FGR(ft) + 4;
    if (dbl) ppc_lfd(c, 1, offS, JR_CPU);
    else ppc_lfs(c, 1, offS, JR_CPU);

    if (cmp)
    {
        // No NaN: no exception, cause cleared, C = the condition.
        u32 cond = funct & 15, set[2], nSet = 0, over, i;
        if (dbl) ppc_lfd(c, 2, offT, JR_CPU);
        else ppc_lfs(c, 2, offT, JR_CPU);
        ppc_fcmpu(c, 0, 1, 2);
        slow[(*nSlow)++] = ppc_bc_fwd(c, 12, 0, PPC_UN);
        ppc_rlwinm(c, 5, 5, 0, 20, 13);   // cause
        ppc_rlwinm(c, 5, 5, 0, 9, 7);     // C
        if (cond & 4) set[nSet++] = ppc_bc_fwd(c, 12, 0, PPC_LT);
        if (cond & 2) set[nSet++] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        if (nSet)
        {
            over = ppc_b_fwd(c);
            for (i = 0; i < nSet; i++) ppc_patch_here(c, set[i]);
            ppc_oris(c, 5, 5, 0x0080);
            ppc_patch_here(c, over);
        }
        ppc_stw(c, 5, OFF_FCR31, JR_CPU);
        return 1;
    }

    // Operand checks against the smallest normal number of the source format.
    ppc_lfd(c, 4, OFF_FPMIN(dbl), JR_CPU);
    ppc_fsub(c, 5, 4, 4);
    fp_check(g, 1, slow, nSlow);
    if (funct == 0x05 || funct == 0x07)
    {
        // ABS, NEG: no exception for these operands; the cause is cleared.
        if (funct == 0x05) ppc_fabs(c, 1, 1);
        else ppc_fneg(c, 1, 1);
        ppc_rlwinm(c, 5, 5, 0, 20, 13);
        ppc_stw(c, 5, OFF_FCR31, JR_CPU);
        if (dbl) ppc_stfd(c, 1, OFF_FGR(fd), JR_CPU);
        else
        {
            fp_store32_zext(g, fd);
            ppc_stfs(c, 1, OFF_FGR(fd) + 4, JR_CPU);
        }
        return 1;
    }
    if (cvtd)
    {
        // Exact: a normal single is a normal double, no flag.
        ppc_rlwinm(c, 5, 5, 0, 20, 13);
        ppc_stw(c, 5, OFF_FCR31, JR_CPU);
        ppc_stfd(c, 1, OFF_FGR(fd), JR_CPU);
        return 1;
    }
    if (toint)
    {
        if (funct == 0x0D) ppc_fctiwz(c, 1, 1);
        else
        {
            u32 nearest = ppc_bc_fwd(c, 12, 0, PPC_EQ), join;
            ppc_fctiwz(c, 1, 1);
            join = ppc_b_fwd(c);
            ppc_patch_here(c, nearest);
            ppc_fctiw(c, 1, 1);
            ppc_patch_here(c, join);
        }
        fp_flags(g, slow, nSlow);   // NaN, infinite or out of range: VXCVI
        fp_inexact(g, slow, nSlow);
        fp_store32_zext(g, fd);
        ppc_addi(c, 8, JR_CPU, OFF_FGR(fd) + 4);
        ppc_stfiwx(c, 1, 0, 8);
        return 1;
    }
    if (arith)
    {
        if (dbl) ppc_lfd(c, 2, offT, JR_CPU);
        else ppc_lfs(c, 2, offT, JR_CPU);
        fp_check(g, 2, slow, nSlow);
        switch (funct)
        {
        case 0: if (dbl) ppc_fadd(c, 1, 1, 2); else ppc_fadds(c, 1, 1, 2); break;
        case 1: if (dbl) ppc_fsub(c, 1, 1, 2); else ppc_fsubs(c, 1, 1, 2); break;
        case 2: if (dbl) ppc_fmul(c, 1, 1, 2); else ppc_fmuls(c, 1, 1, 2); break;
        default: if (dbl) ppc_fdiv(c, 1, 1, 2); else ppc_fdivs(c, 1, 1, 2); break;
        }
    }
    else
    {
        ppc_frsp(c, 1, 1);   // CVT.S.D
        ppc_lfd(c, 4, OFF_FPMIN(0), JR_CPU);
    }
    fp_flags(g, slow, nSlow);
    // A denormal result (exact, so without UX): the slow path.
    fp_check(g, 1, slow, nSlow);
    fp_inexact(g, slow, nSlow);
    if (dbl && !cvts) ppc_stfd(c, 1, OFF_FGR(fd), JR_CPU);
    else
    {
        fp_store32_zext(g, fd);
        ppc_stfs(c, 1, OFF_FGR(fd) + 4, JR_CPU);
    }
    return 1;
}

static int emit_fpu(Gen *g, u32 op, u32 pc)
{
    u32 slow[16], nSlow = 0, done;
    if (!emit_fpu_fast(g, op, slow, &nSlow)) return 0;
    done = ppc_b_fwd(&g->c);
    emit_slow_tail(g, pc, slow, nSlow, done);
    return 1;
}

// ---- Branches ----
// Computes R_COND (1: taken) and R_TARGET for a native branch; writes the link.
// Returns 0 when the branch is not handled natively (BC1x).
static int emit_branch_head(Gen *g, u32 op, u32 pc, int *likely, int *dynamicTarget, u64 *target)
{
    H64PpcCode *c = &g->c;
    u32 opc = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, at;
    // 64-bit arithmetic on the sign-extended pc, as the interpreter does.
    u64 branchTarget = sext32(pc) + 4 + ((u64)(s64)(s16)(op & 0xFFFF) << 2);
    *likely = 0;
    *dynamicTarget = 0;
    *target = branchTarget;
    switch (opc)
    {
    case 0x00:   // JR, JALR
        load_gpr(g, R_TARGET, rs);
        if ((op & 63) == 0x09 && rd != 0)
        {
            ppc_li64(c, 3, (sext32(pc) + 8));
            store_gpr(g, 3, rd);
        }
        ppc_li(c, R_COND, 1);
        *dynamicTarget = 1;
        return 1;
    case 0x02: case 0x03:   // J, JAL
        *target = ((sext32(pc) + 4) & 0xFFFFFFFFF0000000ull) | ((u64)(op & 0x03FFFFFF) << 2);
        if (opc == 0x03) { ppc_li64(c, 3, (sext32(pc) + 8)); store_gpr(g, 3, 31); }
        ppc_li(c, R_COND, 1);
        return 1;
    case 0x01:   // REGIMM: BLTZ BGEZ BLTZL BGEZL BLTZAL BGEZAL BLTZALL BGEZALL
    {
        int ge = rt & 1;
        *likely = (rt & 2) != 0;
        load_gpr(g, 3, rs);
        ppc_cmpdi(c, 0, 3, 0);
        ppc_li(c, R_COND, 1);
        at = ppc_bc_fwd(c, ge ? 4 : 12, 0, PPC_LT);   // BGEZ: taken unless < 0; BLTZ: taken if < 0
        ppc_li(c, R_COND, 0);
        ppc_patch_here(c, at);
        if (rt & 0x10) { ppc_li64(c, 3, (sext32(pc) + 8)); store_gpr(g, 3, 31); }   // BxxAL: link even when not taken
        return 1;
    }
    case 0x04: case 0x05: case 0x14: case 0x15:   // BEQ BNE BEQL BNEL
        *likely = opc >= 0x14;
        load_gpr(g, 3, rs);
        load_gpr(g, 4, rt);
        ppc_cmpd(c, 0, 3, 4);
        ppc_li(c, R_COND, 1);
        at = ppc_bc_fwd(c, (opc & 1) ? 4 : 12, 0, PPC_EQ);   // BEQ: skip if equal; BNE: skip if not equal
        ppc_li(c, R_COND, 0);
        ppc_patch_here(c, at);
        return 1;
    case 0x11:   // BC1F BC1T BC1FL BC1TL: COP1 must be usable, else the interpreter (bc1Slow)
        if (rs != 0x08) return 0;
        ppc_ld(c, 3, OFF_COP0(CP0_STATUS), JR_CPU);
        ppc_andis_(c, 3, 3, 0x2000);
        g->bc1Slow = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        g->hasBc1Slow = 1;
        *likely = (rt & 2) != 0;
        ppc_lwz(c, 3, OFF_FCR31, JR_CPU);
        ppc_rlwinm(c, R_COND, 3, 9, 31, 31);        // C
        if (!(rt & 1)) ppc_xori(c, R_COND, R_COND, 1);
        return 1;
    case 0x06: case 0x07: case 0x16: case 0x17:   // BLEZ BGTZ BLEZL BGTZL
        *likely = opc >= 0x16;
        load_gpr(g, 3, rs);
        ppc_cmpdi(c, 0, 3, 0);
        ppc_li(c, R_COND, 1);
        at = ppc_bc_fwd(c, (opc & 1) ? 12 : 4, 0, PPC_GT);   // BGTZ: skip if > 0; BLEZ: skip unless > 0
        ppc_li(c, R_COND, 0);
        ppc_patch_here(c, at);
        return 1;
    }
    return 0;
}

// Stores pc = (R_COND ? target : fallthrough), nextPc = pc + 4.
static void store_branch_pc(Gen *g, int dynamicTarget, u64 target, u32 fallthrough)
{
    H64PpcCode *c = &g->c;
    u32 at, done;
    ppc_cmpdi(c, 0, R_COND, 0);
    at = ppc_bc_fwd(c, 12, 0, PPC_EQ);   // not taken
    if (dynamicTarget) ppc_mr(c, 3, R_TARGET);
    else ppc_li64(c, 3, target);
    done = ppc_b_fwd(c);
    ppc_patch_here(c, at);
    ppc_li64(c, 3, sext32(fallthrough));
    ppc_patch_here(c, done);
    ppc_std(c, 3, OFF_PC, JR_CPU);
    ppc_addi(c, 3, 3, 4);
    ppc_std(c, 3, OFF_NEXTPC, JR_CPU);
}

// MFC0, DMFC0 (cop0_read in h64_cpu.cpp), except Random. Count is computed
// from the cycles at this instruction (synced cycles + pending ones).
static int emit_mfc0(Gen *g, u32 op)
{
    H64PpcCode *c = &g->c;
    u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
    if ((op >> 26) != 0x10 || (rs != 0 && rs != 1) || rd == CP0_RANDOM) return 0;
    if (rt == 0) return 1;
    if (rd == CP0_COUNT)
    {
        ppc_ld(c, 3, OFF_CYCLES, JR_CPU);
        ppc_addi(c, 3, 3, (s32)(g->pending * g->cpi));
        ppc_srdi(c, 3, 3, 1);
        ppc_lwz(c, 4, (s32)offsetof(H64Cpu, countOffset), JR_CPU);
        ppc_add(c, 3, 3, 4);
        if (rs == 0) ppc_extsw(c, 3, 3);
        else ppc_clrldi(c, 3, 3, 32);
    }
    else
    {
        int latch = rd == 7 || (rd >= 21 && rd <= 25) || rd == 31;
        ppc_ld(c, 3, latch ? (s32)offsetof(H64Cpu, cop0Latch) : OFF_COP0(rd), JR_CPU);
        if (rs == 0) ppc_extsw(c, 3, 3);
    }
    store_gpr(g, 3, rt);
    return 1;
}

static int emit_native(Gen *g, u32 op, u32 pc)
{
    if (emit_alu(g, op)) { g->pending++; return 1; }
    if (emit_mfc0(g, op)) { g->pending++; return 1; }
    if ((op >> 26) == 0x2F) { g->pending++; return 1; }   // CACHE: caches are not emulated
    if (emit_ovf(g, op, pc)) return 1;
    if (emit_fpu(g, op, pc)) return 1;
    return emit_mem(g, op, pc);
}

// A native branch and its delay slot; the block ends after them.
static void emit_branch_body(Gen *g, u32 op, u32 pc, u32 ds);

// A native branch and its delay slot. BC1 with COP1 unusable falls back to the
// interpreter for the branch and the slot (it raises the exception).
static void emit_branch(Gen *g, u32 op, u32 pc, u32 ds)
{
    u32 pend0 = g->pending;
    g->hasBc1Slow = 0;
    emit_branch_body(g, op, pc, ds);
    if (g->hasBc1Slow)
    {
        ppc_patch_here(&g->c, g->bc1Slow);
        g->pending = pend0;
        call_interp(g, pc);
        call_interp_raw(g);
        exit_block(g);
    }
}

static void emit_branch_body(Gen *g, u32 op, u32 pc, u32 ds)
{
    H64PpcCode *c = &g->c;
    int likely, dynamicTarget;
    u64 target;
    u32 dsPc = pc + 4;
    emit_branch_head(g, op, pc, &likely, &dynamicTarget, &target);
    g->pending++;   // the branch itself
    if (likely)
    {
        // Not taken: the delay slot is skipped, the block ends at pc + 8.
        u32 takenAt;
        ppc_cmpdi(c, 0, R_COND, 0);
        takenAt = ppc_bc_fwd(c, 4, 0, PPC_EQ);   // taken: go on with the slot
        sync_to(g, g->pending, pc + 8);
        link_exit(g, pc + 8);
        ppc_patch_here(c, takenAt);
    }
    // The delay slot.
    if (g->native && !is_branch(ds) && !ends_block(ds) && (emit_alu(g, ds) ? (g->pending++, 1) : 0))
    {
        add_to(g, OFF_CYCLES, (s32)(g->pending * g->cpi));
        add_to(g, OFF_INSNS, (s32)g->pending);
        store_branch_pc(g, dynamicTarget, target, dsPc + 4);
        ppc_li(c, 3, 0);
        ppc_stw(c, 3, OFF_BRANCH, JR_CPU);
        g->pending = 0;
        branch_exit(g, dynamicTarget, target, dsPc + 4);
        return;
    }
    // A load or store in the slot: native fast path; its slow path is the
    // interpreted slot below.
    if (g->native && !is_branch(ds) && !ends_block(ds))
    {
        u32 slow[16], nSlow = 0, i;
        u32 pend = g->pending;
        if (emit_mem_fast(g, ds, slow, &nSlow) || emit_ovf_fast(g, ds, slow, &nSlow) || emit_fpu_fast(g, ds, slow, &nSlow))
        {
            add_to(g, OFF_CYCLES, (s32)((pend + 1) * g->cpi));
            add_to(g, OFF_INSNS, (s32)(pend + 1));
            store_branch_pc(g, dynamicTarget, target, dsPc + 4);
            ppc_li(c, 3, 0);
            ppc_stw(c, 3, OFF_BRANCH, JR_CPU);
            branch_exit(g, dynamicTarget, target, dsPc + 4);
            for (i = 0; i < nSlow; i++) ppc_patch_here(c, slow[i]);
            g->pending = pend;
        }
    }
    // Otherwise the interpreter runs the slot, as it would after the branch:
    // pc = slot, nextPc = target or fall-through, branchPending = 1.
    add_to(g, OFF_CYCLES, (s32)(g->pending * g->cpi));
    add_to(g, OFF_INSNS, (s32)g->pending);
    g->pending = 0;
    store_branch_pc(g, dynamicTarget, target, dsPc + 4);   // pc = target/fallthrough (temporarily)
    ppc_ld(c, 3, OFF_PC, JR_CPU);
    ppc_std(c, 3, OFF_NEXTPC, JR_CPU);                      // nextPc = target/fallthrough
    ppc_li64(c, 3, sext32(dsPc));
    ppc_std(c, 3, OFF_PC, JR_CPU);                          // pc = the slot
    ppc_li(c, 3, 1);
    ppc_stw(c, 3, OFF_BRANCH, JR_CPU);
    call_interp_raw(g);
    exit_block(g);
}

// ---- Idle loops ----
// A block that branches back to its own start without side effects can be
// skipped by whole iterations: the result is the same as running them.
// Recognised: "b self; nop" style loops whose condition registers the loop
// doesn't write, and RDRAM polls "lw rX, off(base); beq/bne rX, rY, self; nop".
static int plain_branch_to(u32 op, u32 pc, u32 target)
{
    u32 opc = op >> 26, rt = (op >> 16) & 31;
    u64 off = sext32(pc) + 4 + ((u64)(s64)(s16)(op & 0xFFFF) << 2);
    if (opc == 0x02) return (((pc + 4) & 0xF0000000u) | ((op & 0x03FFFFFF) << 2)) == target;   // J
    if ((opc >= 0x04 && opc <= 0x07) || (opc == 0x01 && rt <= 0x01)) return off == sext32(target);
    return 0;
}

static void classify_idle(H64JitBlock *b, const u32 *ops, u32 n)
{
    u32 pc = b->vpc;
    b->idle = 0;
    if (n == 2 && ops[1] == 0 && plain_branch_to(ops[0], pc, pc))
        b->idle = 1;   // the loop writes no register: its condition can't change
    else if (n == 3 && ops[2] == 0 && plain_branch_to(ops[1], pc + 4, pc) && ((ops[1] >> 26) == 0x04 || (ops[1] >> 26) == 0x05))
    {
        u32 lop = ops[0] >> 26, rX = (ops[0] >> 16) & 31, base = (ops[0] >> 21) & 31;
        u32 bs = (ops[1] >> 21) & 31, bt = (ops[1] >> 16) & 31;
        u32 size = lop == 0x20 || lop == 0x24 ? 1 : lop == 0x21 || lop == 0x25 ? 2 : lop == 0x23 || lop == 0x27 ? 4 : 0;
        if (size && rX != 0 && rX != base && (bs == rX || bt == rX))
        {
            b->idle = 2;
            b->pollBase = base;
            b->pollOff = (s32)(s16)(ops[0] & 0xFFFF);
            b->pollSize = size;
        }
    }
}

// ---- Block compilation ----
H64JitBlock *h64_jit_compile(H64System *sys, u32 pc, u32 paddr)
{
    H64Jit *j = sys->jit;
    H64JitBlock *b;
    Gen g;
    u32 ops[H64_JIT_MAX_INSNS], n = 0, i, bodyAt;
    u32 pageEnd = (paddr | 0xFFF) + 1;
    int endsWithBranch = 0;
    u8 *start;

    // Formation: stop at the page end, after a branch's delay slot, after a
    // COP0 instruction, or at the size limit.
    while (n < H64_JIT_MAX_INSNS && paddr + n * 4 < pageEnd)
    {
        u32 op = h64_load_be32(sys->rdram + paddr + n * 4);
        if (is_branch(op))
        {
            // The branch and its delay slot stay together: end the block
            // before the branch when the slot is on the next page or past the size limit.
            if (paddr + n * 4 + 4 >= pageEnd || n + 2 > H64_JIT_MAX_INSNS) break;
            ops[n++] = op;
            ops[n] = h64_load_be32(sys->rdram + paddr + n * 4);
            n++;
            endsWithBranch = 1;
            break;
        }
        ops[n++] = op;
        if (ends_block(op)) break;
    }
    if (n == 0) return 0;

    if (j->blockCount >= j->blockCap || j->memUsed + MAX_BLOCK_BYTES + 64 > j->memSize)
        h64_jit_reset(sys);

    // ELFv1: a 3-doubleword function descriptor precedes the code.
    j->memUsed = (j->memUsed + 15) & ~15u;
    start = j->mem + j->memUsed;
#if defined(H64_JIT_ABI_ELFV1)
    start += 32;
#endif
    memset(&g, 0, sizeof(g));
    g.sys = sys;
    g.c.buf = (u32 *)start;
    g.c.cap = MAX_BLOCK_BYTES / 4;
    g.pc0 = pc;
    g.paddr0 = paddr;
    g.cpi = sys->cpu.cpi ? sys->cpu.cpi : 1;
    g.native = h64_jit_kernel_mode(&sys->cpu) && !j->noNative;
    g.fpu = g.native && !j->noFpu && h64_fenv_reliable();
    sys->cpu.jitFpMin[0] = ldexp(1.0, -126);
    sys->cpu.jitFpMin[1] = ldexp(1.0, -1022);

    emit_prologue(&g);
    bodyAt = g.c.pos;
    for (i = 0; i < n; i++)
    {
        u32 ipc = pc + i * 4, op = ops[i];
        if (endsWithBranch && i == n - 2)
        {
            if (g.native && ((op >> 26) != 0x11 || ((op >> 21) & 31) == 0x08))
            {
                emit_branch(&g, op, ipc, ops[i + 1]);
                j->stats.nativeInsns++;
            }
            else
            {
                // Branch and slot through the interpreter: after the branch the
                // state is the interpreter's (pc = slot, nextPc, branchPending).
                call_interp(&g, ipc);
                call_interp_raw(&g);
                j->stats.helperInsns += 2;
            }
            exit_block(&g);
            break;
        }
        if (g.native && !ends_block(op) && emit_native(&g, op, ipc))
        {
            j->stats.nativeInsns++;
            continue;
        }
        call_interp(&g, ipc);
        j->stats.helperInsns++;
    }
    if (!endsWithBranch)
    {
        sync_to(&g, g.pending, pc + n * 4);
        if (ends_block(ops[n - 1])) exit_block(&g);   // COP0: the mode may have changed
        else link_exit(&g, pc + n * 4);
    }
    for (i = 0; i < g.nExits; i++) ppc_patch_here(&g.c, g.exits[i]);
    emit_epilogue(&g);
    if (g.c.overflow)
    {
        H64_ERROR("[jit] block at %08X too large", pc);
        return 0;
    }

    b = &j->blocks[j->blockCount++];
    memset(b, 0, sizeof(*b));
    b->vpc = pc;
    b->paddr = paddr;
    b->insns = n;
    b->kernel = g.native;
    b->body = (u32 *)start + bodyAt;
    b->linkHead = -1;
    b->valid = 1;
    if (g.native) classify_idle(b, ops, n);
#if defined(H64_JIT_ABI_ELFV1)
    {
        u64 *desc = (u64 *)(start - 32);
        desc[0] = (u64)(uintptr_t)start;
        desc[1] = 0;
        desc[2] = 0;
        b->fn = (H64JitFn)(void *)desc;
    }
#else
    b->fn = (H64JitFn)(void *)start;
#endif
    j->memUsed = (u32)((start - j->mem) + g.c.pos * 4);
    if (j->flushIcache) j->flushIcache(start, g.c.pos * 4);

    b->hashNext = j->hash[(pc >> 2) & 8191];
    j->hash[(pc >> 2) & 8191] = b;
    b->pageNext = j->pageHead[paddr >> 12];
    j->pageHead[paddr >> 12] = b;
    h64_jit_code_map(j, b, 1);
    j->stats.blocksCompiled++;
    H64_DEBUG("[jit] block %08X (phys %06X): %u instructions, %u bytes, first %08X", pc, paddr, n, g.c.pos * 4, ops[0]);
    return b;
}
