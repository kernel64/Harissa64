// Harissa64 V2 - recompiler code generation.
//
// Block layout (64-bit PowerPC):
//   prologue: stdu/stwu r1,-FRAME(r1); mflr r0; save r0 and r14..r31 in the
//             top of the frame, f14..f31 below them (152..295); r31 = sys,
//             r30 = &sys->cpu, r29 = RDRAM, r28 = the interpreter helper
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
#include <stdio.h>
#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_fenv.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"

// 112-byte ELFv1 area, 112..151 rtSlow's saves, 152..295 f14..f31,
// 296..439 r14..r31, 440 LR.
#define FRAME 448
#define FRAME_FPR(r) (152 + 8 * ((s32)(r) - 14))

// Fields generated code reaches with 16-bit displacements.
static_assert(offsetof(H64System, mi) + 16 < 32768, "sys->mi out of displacement range");
static_assert(offsetof(H64System, jit) < 32768, "sys->jit out of displacement range");
static_assert(offsetof(H64Jit, codeMap) < 32768, "jit->codeMap out of displacement range");
static_assert(offsetof(H64Jit, indBody) < 32768, "jit->indBody out of displacement range");
#define MAX_BLOCK_BYTES (H64_JIT_MAX_INSNS * 320 + 2048)
// The cold part: each slow path writes back and reloads cached registers.
#define MAX_COLD_BYTES (H64_JIT_MAX_INSNS * 1024 + 4096)

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
    {
        u64 t0 = h64_prof_now(sys);
        u64 nested = h64_prof_nested(sys);
        if (haveOp) h64_cpu_step_op(sys, op);
        else h64_cpu_step(sys);
        h64_prof_add_outer(sys, H64_PROF_HELPER, t0, nested);
    }
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
// MIPS GPRs cached in host registers r14..r25 inside a block: loaded on first
// use, written back (the dirty ones) before every helper call and every exit,
// so H64Cpu is exact whenever C code or another block looks at it. No register
// is evicted inside one MIPS instruction (begin_insn frees slots first): a slow
// path writes back the dirty set of the instruction's start (pre) and, after
// the call, reloads every register mapped at the instruction's end (rc).
//
// COP1 registers (FGRs, by fgr[] index) are cached the same way in host FPRs
// once the block knows Status.CU1 and FR (fpu_guard): f14..f31 (saved by the
// shared prologue) then f6..f13 (volatile: a C call clobbers them, so slow
// paths write them back and reload them). An entry holds either the whole
// 64-bit register as a double (lfd/stfd, bit-exact) or its low word as a
// single (lfs/stfs, bit-exact for every pattern); a single from an arithmetic
// result also zeroes the high word when written back (fhiz), one from LWC1
// keeps it. A read in the other format, or any access through memory (moves,
// CVT from W, odd registers with FR = 0, the interpreter) writes the entry
// back first.
#define RC_SLOTS 12
#define RC_FIRST 14
#define FC_SLOTS 26
#define FC_FREE 3           // an instruction maps at most 3 FGRs
#define FC_T_DBL 0x20       // rtFcStore/rtFcLoad table bytes: FGR | these, 0xFF unused
#define FC_T_HIZ 0x40
#define FC_TABLE_WORDS ((FC_SLOTS + 3) / 4)
struct RegCache
{
    s8 host[32];            // MIPS register -> slot, or -1
    u8 mips[RC_SLOTS];      // slot -> MIPS register (0: free)
    u32 age[RC_SLOTS];
    u32 dirty;              // MIPS registers newer in the host register
    s8 fhost[32];           // FGR -> FPR slot, or -1
    s8 fidx[FC_SLOTS];      // FPR slot -> FGR, or -1
    u32 fage[FC_SLOTS];
    u32 fdirty;             // FGRs newer in the host FPR
    u32 fdbl;               // FGRs held as doubles (else singles: the low word)
    u32 fhiz;               // singles whose write-back also zeroes the high word
};

static u32 fc_reg(u32 slot) { return slot < 18 ? 14 + slot : 6 + (slot - 18); }

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
    int cacheOn;      // GPR caching (native blocks)
    RegCache rc, pre; // the current state; the state at the start of the current instruction
    RegCache bc1Pre;  // the state at the branch (BC1 fallback)
    u32 tick;
    // COP1 state checked once per block (fpu_guard): Status.CU1 set and FR =
    // fpuFr for the rest of the block; FCR31's rounding mode nearest until a CTC1.
    int fpuKnown, rmNearest;
    u32 fpuFr;
    int tailExit;     // the current instruction's slow path leaves the block afterwards (a guard failed)
    u32 ftouch;       // FGRs the current instruction reads or writes through the FPR cache
};

static void add_exit(Gen *g, u32 at) { if (g->nExits < H64_JIT_MAX_INSNS * 4) g->exits[g->nExits++] = at; }

static void rc_reset(RegCache *r)
{
    memset(r->host, -1, sizeof(r->host));
    memset(r->mips, 0, sizeof(r->mips));
    memset(r->age, 0, sizeof(r->age));
    r->dirty = 0;
    memset(r->fhost, -1, sizeof(r->fhost));
    memset(r->fidx, -1, sizeof(r->fidx));
    memset(r->fage, 0, sizeof(r->fage));
    r->fdirty = 0;
    r->fdbl = 0;
    r->fhiz = 0;
}

// ---- FPR cache: code for one entry of state r ----
// *zero: r0 already holds 0 in this sequence (singles zeroing the high word).
static void fc_store(Gen *g, const RegCache *r, u32 m, int *zero)
{
    u32 f = fc_reg((u32)r->fhost[m]);
    if (r->fdbl & (1u << m)) ppc_stfd(&g->c, f, OFF_FGR(m), JR_CPU);
    else
    {
        if (r->fhiz & (1u << m))
        {
            if (!*zero) ppc_li(&g->c, 0, 0);
            *zero = 1;
            ppc_stw(&g->c, 0, OFF_FGR(m), JR_CPU);
        }
        ppc_stfs(&g->c, f, OFF_FGR(m) + 4, JR_CPU);
    }
}

static void fc_load(Gen *g, const RegCache *r, u32 m)
{
    u32 f = fc_reg((u32)r->fhost[m]);
    if (r->fdbl & (1u << m)) ppc_lfd(&g->c, f, OFF_FGR(m), JR_CPU);
    else ppc_lfs(&g->c, f, OFF_FGR(m) + 4, JR_CPU);
}

// Stores the dirty FGRs of state r that are in `mask`.
static void fc_writeback_mask(Gen *g, const RegCache *r, u32 mask)
{
    u32 m;
    int zero = 0;
    for (m = 0; m < 32; m++)
        if (r->fhost[m] >= 0 && (r->fdirty & mask & (1u << m))) fc_store(g, r, m, &zero);
}

// Stores the dirty registers of state r (code only; the generator state is unchanged).
static void rc_writeback_gpr(Gen *g, const RegCache *r)
{
    u32 m;
    for (m = 1; m < 32; m++)
        if (r->host[m] >= 0 && (r->dirty & (1u << m)))
            ppc_std(&g->c, RC_FIRST + (u32)r->host[m], OFF_GPR(m), JR_CPU);
}

static void rc_writeback(Gen *g, const RegCache *r)
{
    rc_writeback_gpr(g, r);
    fc_writeback_mask(g, r, 0xFFFFFFFFu);
}

// Loads every GPR mapped in state r from H64Cpu (after a helper call).
static void rc_reload(Gen *g, const RegCache *r)
{
    u32 m;
    for (m = 1; m < 32; m++)
        if (r->host[m] >= 0) ppc_ld(&g->c, RC_FIRST + (u32)r->host[m], OFF_GPR(m), JR_CPU);
}

// Helper call on the main path: write back, then forget everything.
static void rc_flush(Gen *g)
{
    if (!g->cacheOn) return;
    rc_writeback(g, &g->rc);
    rc_reset(&g->rc);
}

// ---- FPR cache: generator state ----
static int fc_on(Gen *g) { return g->cacheOn && g->fpuKnown && !g->sys->jit->noFpCache; }

// Forgets FGR m (no code: the caller overwrites or already stored it).
static void fc_drop(Gen *g, u32 m)
{
    RegCache *r = &g->rc;
    u32 bit = 1u << m;
    g->ftouch |= bit;
    if (r->fhost[m] < 0) return;
    r->fidx[(u32)r->fhost[m]] = -1;
    r->fhost[m] = -1;
    r->fdirty &= ~bit;
    r->fdbl &= ~bit;
    r->fhiz &= ~bit;
}

// H64Cpu gets FGR m's value (the entry stays, clean).
static void fc_sync(Gen *g, u32 m)
{
    RegCache *r = &g->rc;
    int zero = 0;
    g->ftouch |= 1u << m;
    if (r->fhost[m] < 0 || !(r->fdirty & (1u << m))) return;
    fc_store(g, r, m, &zero);
    r->fdirty &= ~(1u << m);
}

static void fc_flush(Gen *g, u32 m)
{
    fc_sync(g, m);
    fc_drop(g, m);
}

// A slot for FGR m, free now and at the start of the instruction (a slow
// path writes back the start's state from the host registers: they must
// still hold it). begin_insn keeps FC_FREE such slots.
static u32 fc_alloc(Gen *g, u32 m, int dbl)
{
    RegCache *r = &g->rc;
    u32 i, bit = 1u << m;
    for (i = 0; i < FC_SLOTS; i++)
        if (r->fidx[i] < 0 && g->pre.fidx[i] < 0) break;
    if (i == FC_SLOTS) { g->c.overflow = 1; i = 0; }
    r->fidx[i] = (s8)m;
    r->fhost[m] = (s8)i;
    r->fdirty &= ~bit;
    r->fhiz &= ~bit;
    if (dbl) r->fdbl |= bit;
    else r->fdbl &= ~bit;
    return i;
}

// The host FPR holding FGR m as a double (dbl) or a single, loaded if needed.
static u32 fc_src(Gen *g, u32 m, int dbl)
{
    RegCache *r = &g->rc;
    u32 i;
    if (r->fhost[m] >= 0 && (((r->fdbl >> m) & 1) != (u32)dbl)) fc_flush(g, m);   // the other format
    if (r->fhost[m] < 0)
    {
        fc_alloc(g, m, dbl);
        fc_load(g, r, m);
    }
    i = (u32)r->fhost[m];
    r->fage[i] = ++g->tick;
    g->ftouch |= 1u << m;
    return fc_reg(i);
}

// The host FPR receiving a whole new value of FGR m (a double, or a single
// with hiz: the high word becomes 0). Emit it after the instruction's last
// branch to its slow path.
static u32 fc_dst(Gen *g, u32 m, int dbl, int hiz)
{
    RegCache *r = &g->rc;
    u32 i, bit = 1u << m;
    if (r->fhost[m] < 0) fc_alloc(g, m, dbl);
    i = (u32)r->fhost[m];
    if (dbl) r->fdbl |= bit;
    else r->fdbl &= ~bit;
    if (!dbl && hiz) r->fhiz |= bit;
    else r->fhiz &= ~bit;
    r->fdirty |= bit;
    r->fage[i] = ++g->tick;
    g->ftouch |= bit;
    return fc_reg(i);
}

// At the start of each MIPS instruction: at least 4 free slots (an instruction
// uses 3 registers at most, a branch-and-link 3), evicting the least recently used.
static void begin_insn(Gen *g)
{
    if (g->cacheOn)
    {
        u32 i, free = 0;
        for (i = 0; i < RC_SLOTS; i++) if (!g->rc.mips[i]) free++;
        while (free < 4)
        {
            u32 best = 0, bestAge = 0xFFFFFFFFu, m;
            for (i = 0; i < RC_SLOTS; i++)
                if (g->rc.mips[i] && g->rc.age[i] < bestAge) { bestAge = g->rc.age[i]; best = i; }
            m = g->rc.mips[best];
            if (g->rc.dirty & (1u << m)) ppc_std(&g->c, RC_FIRST + best, OFF_GPR(m), JR_CPU);
            g->rc.dirty &= ~(1u << m);
            g->rc.host[m] = -1;
            g->rc.mips[best] = 0;
            free++;
        }
        free = 0;
        for (i = 0; i < FC_SLOTS; i++) if (g->rc.fidx[i] < 0) free++;
        while (free < FC_FREE)
        {
            u32 best = 0, bestAge = 0xFFFFFFFFu;
            for (i = 0; i < FC_SLOTS; i++)
                if (g->rc.fidx[i] >= 0 && g->rc.fage[i] < bestAge) { bestAge = g->rc.fage[i]; best = i; }
            fc_flush(g, (u32)g->rc.fidx[best]);
            free++;
        }
    }
    g->pre = g->rc;
    g->tailExit = 0;
    g->ftouch = 0;
}

static u32 rc_slot(Gen *g, u32 mips, int load)
{
    RegCache *r = &g->rc;
    u32 i;
    if (r->host[mips] < 0)
    {
        for (i = 0; i < RC_SLOTS && r->mips[i]; i++) {}
        if (i == RC_SLOTS) { g->c.overflow = 1; i = 0; }   // begin_insn guarantees a slot
        r->mips[i] = (u8)mips;
        r->host[mips] = (s8)i;
        if (load) ppc_ld(&g->c, RC_FIRST + i, OFF_GPR(mips), JR_CPU);
    }
    i = (u32)r->host[mips];
    r->age[i] = ++g->tick;
    return RC_FIRST + i;
}

static void load_gpr(Gen *g, u32 reg, u32 mips)
{
    if (mips == 0) ppc_li(&g->c, reg, 0);
    else if (!g->cacheOn) ppc_ld(&g->c, reg, OFF_GPR(mips), JR_CPU);
    else
    {
        u32 h = rc_slot(g, mips, 1);
        if (h != reg) ppc_mr(&g->c, reg, h);
    }
}

static void store_gpr(Gen *g, u32 reg, u32 mips)
{
    if (mips == 0) return;
    if (!g->cacheOn) ppc_std(&g->c, reg, OFF_GPR(mips), JR_CPU);
    else
    {
        u32 h = rc_slot(g, mips, 0);
        if (h != reg) ppc_mr(&g->c, h, reg);
        g->rc.dirty |= 1u << mips;
    }
}

// Source operand: the cached host register itself (no copy), or tmp.
static u32 src_gpr(Gen *g, u32 mips, u32 tmp)
{
    if (mips == 0) { ppc_li(&g->c, tmp, 0); return tmp; }
    if (!g->cacheOn) { ppc_ld(&g->c, tmp, OFF_GPR(mips), JR_CPU); return tmp; }
    return rc_slot(g, mips, 1);
}

// Destination: the host register the result goes to (the cached one, or tmp);
// dst_done marks it written (or stores tmp when not caching).
static u32 dst_gpr(Gen *g, u32 mips, u32 tmp)
{
    if (mips == 0 || !g->cacheOn) return tmp;
    return rc_slot(g, mips, 0);
}

static void dst_done(Gen *g, u32 reg, u32 mips)
{
    if (mips == 0) return;
    if (!g->cacheOn) ppc_std(&g->c, reg, OFF_GPR(mips), JR_CPU);
    else g->rc.dirty |= 1u << mips;
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

static void ppc_branch_to(H64PpcCode *c, const u32 *target, int link)
{
    s32 off = (s32)((const u8 *)target - (const u8 *)(c->buf + c->pos));
    ppc_put(c, 0x48000000u | ((u32)off & 0x03FFFFFCu) | (link ? 1u : 0u));
}

static void exit_block(Gen *g)
{
    if (g->cacheOn) rc_writeback(g, &g->rc);
    add_exit(g, ppc_b_fwd(&g->c));
}

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

static struct { u32 patchAt, target; } s_linkTails[H64_JIT_MAX_INSNS * 2];
static u32 s_nLinkTails;
static int s_tailSetsPc[H64_JIT_MAX_INSNS * 2];   // the tail also writes pc, nextPc, branchPending (compact exits)

static void link_exit(Gen *g, u32 targetPc)
{
    H64PpcCode *c = &g->c;
    u32 patchAt;
    if (!g->native || g->sys->jit->noLink)
    {
        exit_block(g);
        return;
    }
    if (g->cacheOn) rc_writeback(g, &g->rc);   // the next block (or the dispatcher) reads H64Cpu
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
    ppc_put(c, 0x48000000u);   // to the cold part below (emit_cold_links); linked: b <next block's body>
    if (s_nLinkTails < sizeof(s_linkTails) / sizeof(s_linkTails[0]))
    {
        s_linkTails[s_nLinkTails].patchAt = patchAt;
        s_linkTails[s_nLinkTails].target = targetPc;
        s_tailSetsPc[s_nLinkTails] = 0;
        s_nLinkTails++;
    }
    else
        c->overflow = 1;
}

// The compact form, for an exit whose instructions are all done: rtLink adds
// `pending` to the counters and checks events and the run's end; pc, nextPc
// and branchPending are written only by the cold tail (the dispatcher needs
// them; a linked next block does not). About 5 words instead of ~25: DK64's
// and Conker's code did not fit the caches (115 bytes per MIPS instruction).
static void link_exit_n(Gen *g, u32 pending, u32 targetPc)
{
    H64PpcCode *c = &g->c;
    u32 patchAt;
    if (!g->native || g->sys->jit->noLink || s_nLinkTails >= sizeof(s_linkTails) / sizeof(s_linkTails[0]))
    {
        sync_to(g, pending, targetPc);
        exit_block(g);
        return;
    }
    if (g->sys->jit->fullExits)
    {
        sync_to(g, pending, targetPc);
        link_exit(g, targetPc);
        return;
    }
    if (g->cacheOn) rc_writeback(g, &g->rc);
    ppc_li(c, 3, (s32)(pending * g->cpi));
    ppc_li(c, 4, (s32)pending);
    ppc_branch_to(c, g->sys->jit->rtLink, 1);
    ppc_put(c, 0x41820008u);       // beq +8: go on
    ppc_put(c, 0x48000000u);       // an event or the run's end: to the cold tail (patched below)
    patchAt = c->pos;
    ppc_put(c, 0x48000000u);       // to the cold tail; linked: b <next block's body>
    s_linkTails[s_nLinkTails].patchAt = patchAt;
    s_linkTails[s_nLinkTails].target = targetPc;
    s_tailSetsPc[s_nLinkTails] = 1;
    s_nLinkTails++;
}

// Not linked yet (cold, after the body): tell the dispatcher where the exit is
// and where it goes (r5 = sys->jit), then leave.
static void emit_cold_links(Gen *g, H64PpcCode *hot)
{
    H64PpcCode *c = &g->c;
    u32 k;
    for (k = 0; k < s_nLinkTails; k++)
    {
        u32 patchAt = s_linkTails[k].patchAt;
        s32 off = (s32)((const u8 *)(c->buf + c->pos) - (const u8 *)(hot->buf + patchAt));
        if (patchAt < hot->cap) hot->buf[patchAt] = 0x48000000u | ((u32)off & 0x03FFFFFCu);
        if (s_tailSetsPc[k])
        {
            // Compact exits: on an event or the run's end (rtLink said ne) the
            // exit leaves without offering itself for linking: it may be
            // linked already, and offering it again added a link record each
            // time until the table was full (DK64 with the graphics worker:
            // 147000 dispatcher round trips a frame instead of 2000).
            s32 off2 = (s32)((const u8 *)(c->buf + c->pos) - (const u8 *)(hot->buf + patchAt - 1));
            if (patchAt >= 1 && patchAt - 1 < hot->cap) hot->buf[patchAt - 1] = 0x48000000u | ((u32)off2 & 0x03FFFFFCu);
            ppc_li64(c, 3, sext32(s_linkTails[k].target));
            ppc_std(c, 3, OFF_PC, JR_CPU);
            ppc_addi(c, 3, 3, 4);
            ppc_std(c, 3, OFF_NEXTPC, JR_CPU);
            ppc_li(c, 3, 0);
            ppc_stw(c, 3, OFF_BRANCH, JR_CPU);
            add_exit(g, ppc_b_fwd(c));
            off = (s32)((const u8 *)(c->buf + c->pos) - (const u8 *)(hot->buf + patchAt));
            if (patchAt < hot->cap) hot->buf[patchAt] = 0x48000000u | ((u32)off & 0x03FFFFFCu);
            ppc_li64(c, 3, sext32(s_linkTails[k].target));
            ppc_std(c, 3, OFF_PC, JR_CPU);
            ppc_addi(c, 3, 3, 4);
            ppc_std(c, 3, OFF_NEXTPC, JR_CPU);
            ppc_li(c, 3, 0);
            ppc_stw(c, 3, OFF_BRANCH, JR_CPU);
            s_tailSetsPc[k] = 0;
            load_ptr(g, 5, (s32)offsetof(H64System, jit), JR_SYS);
        }
#if defined(H64_JIT_ABI_XBOX)
        ppc_li32u(c, 3, (u32)(uintptr_t)(hot->buf + patchAt));
        ppc_stw(c, 3, (s32)offsetof(H64Jit, lastExit), 5);
#else
        ppc_li64(c, 3, (u64)(uintptr_t)(hot->buf + patchAt));
        ppc_std(c, 3, (s32)offsetof(H64Jit, lastExit), 5);
#endif
        ppc_li32u(c, 3, s_linkTails[k].target);
        ppc_stw(c, 3, (s32)offsetof(H64Jit, lastExitTarget), 5);
        add_exit(g, ppc_b_fwd(c));
    }
    s_nLinkTails = 0;
}

// The exit after a native branch: linked per outcome when the target is fixed.
static void branch_exit(Gen *g, int dynamicTarget, u64 target, u32 fallthrough)
{
    u32 notTaken;
    if (dynamicTarget && g->native && !g->sys->jit->noLink)
    {
        // cpu->pc holds the target: rtIndirect goes on into its block when
        // the dispatcher has seen it (Dr. Mario's main loop: jalr/jr ra every
        // ~30 instructions, each a dispatcher round trip).
        if (g->cacheOn) rc_writeback(g, &g->rc);
        ppc_branch_to(&g->c, g->sys->jit->rtIndirect, 0);
        return;
    }
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
    rc_flush(g);
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
static void call_interp_nc(Gen *g, u32 pc);
static void call_interp(Gen *g, u32 pc)
{
    g->rmNearest = 0;
    rc_flush(g);
    call_interp_nc(g, pc);
}

static void call_interp_nc(Gen *g, u32 pc)
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

// ---- Prologue / epilogue (shared, see h64_jit_emit_runtime) ----
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
    for (r = RC_FIRST; r < 32; r++) ppc_std(c, r, FRAME - 16 - 8 * (s32)(31 - r), 1);
    for (r = 14; r < 32; r++) ppc_stfd(c, r, FRAME_FPR(r), 1);   // the FPR cache's non-volatile registers
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
    for (r = RC_FIRST; r < 32; r++) ppc_ld(c, r, FRAME - 16 - 8 * (s32)(31 - r), 1);
    for (r = 14; r < 32; r++) ppc_lfd(c, r, FRAME_FPR(r), 1);
    ppc_addi(c, 1, 1, FRAME);
    ppc_blr(c);
}

// Shared code at the start of the code memory, rewritten after each reset:
//   enter(sys, body): the prologue, then a jump to the block's body;
//   exit: the epilogue (every block exit branches here);
//   check[store][size]: native load/store checks. r3 = the 64-bit address;
//     returns r4 = the physical address and cr0.eq set when the access can
//     take the fast path: a sign-extended 32-bit KSEG0/KSEG1 address inside
//     the 8 MB of RDRAM ((addr & 0xDF800000) == 0x80000000), aligned, and for
//     stores no block over the 64-byte chunk (codeMap) and no MI repeat mode.
//     Clobbers r5, r6, cr0 and LR (r7 holds Status.FR for COP1 accesses).
void h64_jit_emit_runtime(H64System *sys)
{
    H64Jit *j = sys->jit;
    Gen g;
    u8 *start = j->mem;
    u32 st, sz;
    j->rtCpi = sys->cpu.cpi;
    memset(&g, 0, sizeof(g));
    g.sys = sys;
#if defined(H64_JIT_ABI_ELFV1)
    start += 32;
#endif
    g.c.buf = (u32 *)start;
    g.c.cap = 4096;
    emit_prologue(&g);
    ppc_mtctr(&g.c, 4);
    ppc_put(&g.c, 0x4E800420u);   // bctr
    j->rtExit = g.c.buf + g.c.pos;
    emit_epilogue(&g);
    for (st = 0; st < 2; st++)
        for (sz = 0; sz < 4; sz++)
        {
            H64PpcCode *c = &g.c;
            j->rtCheck[st][sz] = c->buf + c->pos;
            ppc_extsw(c, 4, 3);
            ppc_cmpd(c, 0, 4, 3);
            ppc_put(c, 0x4C820020u);              // bnelr: not a sign-extended 32-bit address
            ppc_andis_(c, 5, 3, 0xDF80);
            ppc_xoris(c, 5, 5, 0x8000);
            ppc_cmpwi(c, 0, 5, 0);
            ppc_put(c, 0x4C820020u);              // bnelr: not KSEG0/1, or beyond 8 MB
            if (sz)
            {
                ppc_andi_(c, 5, 3, (1u << sz) - 1);
                ppc_put(c, 0x4C820020u);          // bnelr: unaligned
            }
            ppc_rlwinm(c, 4, 3, 0, 3, 31);        // physical address
            if (st)
            {
                load_ptr(&g, 6, (s32)offsetof(H64System, jit), JR_SYS);
                load_ptr(&g, 6, (s32)offsetof(H64Jit, codeMap), 6);
                ppc_rlwinm(c, 5, 4, 32 - 5, 5, 30);   // (paddr >> 6) * 2
                ppc_lhzx(c, 5, 6, 5);
                ppc_cmpwi(c, 0, 5, 0);
                ppc_put(c, 0x4C820020u);          // bnelr: code in this chunk
                ppc_lwz(c, 5, (s32)(offsetof(H64System, mi) + offsetof(H64Mi, mode)), JR_SYS);
                ppc_andi_(c, 5, 5, 0x80);         // eq unless MI repeat mode
            }
            ppc_blr(c);
        }
    // Slow path call: r4 = the instruction's pc, r5 = the instruction, r9 =
    // pending instructions before it, r10 = their cycles, r11 = cycles per
    // instruction. Syncs cycles/instructions/pc as call_interp does, runs the
    // interpreter for that instruction, and returns cr0.ne when the block must
    // stop (state left as the interpreter made it), else takes the counters
    // back to the fast path's view (this instruction pending) and returns eq.
    // LR and the counts are kept in the block's frame (free bytes 112..151).
    {
        H64PpcCode *c = &g.c;
        u32 stop;
        j->rtSlow = c->buf + c->pos;
        ppc_mflr(c, 0);
        ppc_std(c, 0, 120, 1);
        ppc_ld(c, 3, OFF_CYCLES, JR_CPU);
        ppc_add(c, 3, 3, 10);
        ppc_add(c, 10, 10, 11);                     // to take back: pending + 1 instructions
        ppc_std(c, 10, 128, 1);
        ppc_std(c, 3, OFF_CYCLES, JR_CPU);
        ppc_ld(c, 3, OFF_INSNS, JR_CPU);
        ppc_add(c, 3, 3, 9);
        ppc_addi(c, 9, 9, 1);
        ppc_std(c, 9, 136, 1);
        ppc_std(c, 3, OFF_INSNS, JR_CPU);
        ppc_extsw(c, 3, 4);
        ppc_std(c, 3, OFF_PC, JR_CPU);
        ppc_addi(c, 3, 3, 4);
        ppc_std(c, 3, OFF_NEXTPC, JR_CPU);
        ppc_li(c, 3, 0);
        ppc_stw(c, 3, OFF_BRANCH, JR_CPU);
        ppc_mr(c, 3, JR_SYS);
        ppc_addi(c, 4, 4, 4);                       // expectedNext
        ppc_li(c, 6, 1);
#if defined(H64_JIT_ABI_ELFV1)
        ppc_ld(c, 0, 0, JR_TMP);
        ppc_ld(c, 2, 8, JR_TMP);
        ppc_mtctr(c, 0);
#else
        ppc_mtctr(c, JR_TMP);
#endif
        ppc_bctrl(c);
        ppc_ld(c, 0, 120, 1);
        ppc_mtlr(c, 0);
        ppc_cmpwi(c, 0, 3, 0);
        stop = ppc_bc_fwd(c, 4, 0, PPC_EQ);         // bne: stop, nothing taken back
        ppc_ld(c, 3, OFF_CYCLES, JR_CPU);
        ppc_ld(c, 4, 128, 1);
        ppc_subf(c, 3, 4, 3);
        ppc_std(c, 3, OFF_CYCLES, JR_CPU);
        ppc_ld(c, 3, OFF_INSNS, JR_CPU);
        ppc_ld(c, 4, 136, 1);
        ppc_subf(c, 3, 4, 3);
        ppc_std(c, 3, OFF_INSNS, JR_CPU);
        ppc_cmpw(c, 0, 3, 3);                       // eq
        ppc_patch_here(c, stop);
        ppc_blr(c);
    }
    // FP operand checks: f4 = smallest normal number of the format, f5 = 0;
    // f1 (and f2) must be normal, zero or infinite (no NaN, no denormal).
    {
        H64PpcCode *c = &g.c;
        u32 dbl, two;
        for (dbl = 0; dbl < 2; dbl++)
            for (two = 0; two < 2; two++)
            {
                u32 fail[4], nFail = 0, k, f;
                j->rtFpCheck[dbl][two] = c->buf + c->pos;
                ppc_lfd(c, 4, OFF_FPMIN(dbl), JR_CPU);
                ppc_fsub(c, 5, 4, 4);
                for (f = 1; f <= (two ? 2u : 1u); f++)
                {
                    u32 ok;
                    ppc_fabs(c, 3, f);
                    ppc_fcmpu(c, 1, 3, 4);
                    fail[nFail++] = ppc_bc_fwd(c, 12, 1, PPC_UN);   // NaN
                    ok = ppc_bc_fwd(c, 4, 1, PPC_LT);                // |x| >= min normal (or infinite)
                    ppc_fcmpu(c, 1, 3, 5);
                    fail[nFail++] = ppc_bc_fwd(c, 4, 1, PPC_EQ);     // denormal
                    ppc_patch_here(c, ok);
                }
                ppc_cmpw(c, 0, 6, 6);   // eq
                ppc_blr(c);
                for (k = 0; k < nFail; k++) ppc_patch_here(c, fail[k]);
                ppc_li(c, 6, 1);
                ppc_cmpwi(c, 0, 6, 0);  // ne
                ppc_blr(c);
            }
        // After an operation (r5 = FCR31): FPSCR VX/OX/UX/ZX -> slow; a denormal
        // result (exact, so without UX) -> slow; an enabled Inexact trap -> slow;
        // else FCR31 gets cause = Inexact (FI) and its flag, and is stored.
        for (dbl = 0; dbl < 3; dbl++)
        {
            u32 fail[4], nFail = 0, k;
            j->rtFpFinish[dbl] = c->buf + c->pos;
            if (j->fastFpu)
            {
                // No FPSCR read (mffs serialises, and the store/load is a
                // load-hit-store: ~100 cycles a COP1 operation; DK64 ran its
                // code at 24 MIPS). Overflow gives the same infinity; NaN and
                // denormal results still take the slow path below.
                ppc_li(c, 7, 0);                  // FI = 0: no Inexact cause
            }
            else
            {
                ppc_mffs(c, 0);
                ppc_stfd(c, 0, OFF_JITSCRATCH, JR_CPU);
                ppc_lwz(c, 6, OFF_JITSCRATCH + 4, JR_CPU);
                ppc_andis_(c, 7, 6, 0x3C00);
                ppc_put(c, 0x4C820020u);          // bnelr
                ppc_rlwinm(c, 7, 6, 15, 31, 31);  // FI
            }
            if (dbl)
            {
                u32 ok;
                ppc_lfd(c, 4, OFF_FPMIN(dbl - 1), JR_CPU);
                ppc_fsub(c, 5, 4, 4);
                ppc_fabs(c, 3, 1);
                ppc_fcmpu(c, 1, 3, 4);
                fail[nFail++] = ppc_bc_fwd(c, 12, 1, PPC_UN);
                ok = ppc_bc_fwd(c, 4, 1, PPC_LT);
                ppc_fcmpu(c, 1, 3, 5);
                fail[nFail++] = ppc_bc_fwd(c, 4, 1, PPC_EQ);
                ppc_patch_here(c, ok);
            }
            ppc_rlwinm(c, 8, 5, 25, 31, 31);      // Enable I
            ppc_and_(c, 8, 8, 7);
            ppc_put(c, 0x4C820020u);              // bnelr: Inexact trap enabled
            ppc_rlwinm(c, 5, 5, 0, 20, 13);       // clear the cause field
            ppc_rlwinm(c, 8, 7, 12, 0, 19);
            ppc_or(c, 5, 5, 8);
            ppc_rlwinm(c, 8, 7, 2, 0, 29);
            ppc_or(c, 5, 5, 8);
            ppc_stw(c, 5, OFF_FCR31, JR_CPU);
            ppc_cmpw(c, 0, 6, 6);                 // eq
            ppc_blr(c);
            for (k = 0; k < nFail; k++) ppc_patch_here(c, fail[k]);
            ppc_li(c, 6, 1);
            ppc_cmpwi(c, 0, 6, 0);
            ppc_blr(c);
        }
    }
    // rtLink (called; r3 = cycles, r4 = instructions of the exit's block): adds
    // them, then cr0.eq if no event and not the run's end fall within the next
    // block (blockEndCycles set), else cr0.ne. Registers only: no reload of
    // what was just stored.
    {
        H64PpcCode *c = &g.c;
        u32 out[2], k;
        j->rtLink = c->buf + c->pos;
        ppc_ld(c, 5, OFF_CYCLES, JR_CPU);
        ppc_ld(c, 6, OFF_INSNS, JR_CPU);
        ppc_add(c, 5, 5, 3);
        ppc_add(c, 6, 6, 4);
        ppc_std(c, 5, OFF_CYCLES, JR_CPU);
        ppc_std(c, 6, OFF_INSNS, JR_CPU);
        ppc_addi(c, 5, 5, (s32)(H64_JIT_MAX_INSNS * sys->cpu.cpi));
        ppc_ld(c, 7, (s32)(offsetof(H64System, sched) + offsetof(H64Scheduler, next)), JR_SYS);
        ppc_cmpld(c, 0, 5, 7);
        out[0] = ppc_bc_fwd(c, 12, 0, PPC_GT);
        load_ptr(&g, 8, (s32)offsetof(H64System, jit), JR_SYS);
        ppc_ld(c, 7, (s32)offsetof(H64Jit, runEnd), 8);
        ppc_cmpld(c, 0, 5, 7);
        out[1] = ppc_bc_fwd(c, 12, 0, PPC_GT);
        ppc_std(c, 5, (s32)offsetof(H64Jit, blockEndCycles), 8);
        ppc_cmpw(c, 0, 5, 5);   // eq
        ppc_blr(c);
        for (k = 0; k < 2; k++) ppc_patch_here(c, out[k]);
        ppc_li(c, 6, 1);
        ppc_cmpwi(c, 0, 6, 0);  // ne
        ppc_blr(c);
    }
    // rtIndirect (branched to, not called): cpu->pc is a jr/jalr target.
    // Like a linked exit: no event and not the run's end within the next
    // block, then the block from the table if it is that pc's; else the exit.
    {
        H64PpcCode *c = &g.c;
        u32 out[8], n = 0, k;
        j->rtIndirect = c->buf + c->pos;
        ppc_ld(c, 3, OFF_PC, JR_CPU);
        ppc_extsw(c, 4, 3);
        ppc_cmpd(c, 0, 4, 3);
        out[n++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);            // bne: not a sign-extended 32-bit pc
        ppc_andis_(c, 5, 3, 0xDF80);
        ppc_xoris(c, 5, 5, 0x8000);
        ppc_cmpwi(c, 0, 5, 0);
        out[n++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);            // not KSEG0/1 RDRAM
        ppc_andi_(c, 5, 3, 3);
        out[n++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);            // misaligned
        ppc_ld(c, 6, OFF_CYCLES, JR_CPU);
        ppc_addi(c, 6, 6, (s32)(H64_JIT_MAX_INSNS * sys->cpu.cpi));
        ppc_ld(c, 7, (s32)(offsetof(H64System, sched) + offsetof(H64Scheduler, next)), JR_SYS);
        ppc_cmpld(c, 0, 6, 7);
        out[n++] = ppc_bc_fwd(c, 12, 0, PPC_GT);           // bgt: an event comes first
        load_ptr(&g, 8, (s32)offsetof(H64System, jit), JR_SYS);
        ppc_ld(c, 7, (s32)offsetof(H64Jit, runEnd), 8);
        ppc_cmpld(c, 0, 6, 7);
        out[n++] = ppc_bc_fwd(c, 12, 0, PPC_GT);           // bgt: the end of the run
        ppc_rlwinm(c, 5, 3, 0, 20, 29);               // (pc & 0xFFC): entry * 4
        load_ptr(&g, 9, (s32)offsetof(H64Jit, indPc), 8);
        ppc_lwzx(c, 10, 9, 5);
        ppc_cmplw(c, 0, 10, 3);
        out[n++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);            // not this pc's block
        load_ptr(&g, 9, (s32)offsetof(H64Jit, indBody), 8);
#if defined(H64_JIT_ABI_XBOX)
        ppc_lwzx(c, 10, 9, 5);
#else
        ppc_rlwinm(c, 5, 3, 1, 19, 28);               // entry * 8
        ppc_ldx(c, 10, 9, 5);
#endif
        ppc_std(c, 6, (s32)offsetof(H64Jit, blockEndCycles), 8);
        ppc_mtctr(c, 10);
        ppc_bctr(c);
        for (k = 0; k < n; k++) ppc_patch_here(c, out[k]);
        ppc_branch_to(c, j->rtExit, 0);
    }
    // rtFcStore / rtFcLoad (called): the FPR cache's slots to or from H64Cpu,
    // described by FC_TABLE_WORDS words right after the call (one byte per
    // slot: 0xFF unused, else the FGR | FC_T_DBL | FC_T_HIZ); they return after
    // the table. Slow paths use them instead of a store or load per register
    // (the cold code filled its half of the code memory on DK64).
    for (st = 0; st < 2; st++)
    {
        H64PpcCode *c = &g.c;
        u32 s;
        if (st) j->rtFcStore = c->buf + c->pos;
        else j->rtFcLoad = c->buf + c->pos;
        ppc_mflr(c, 3);
        if (st) ppc_li(c, 0, 0);
        for (s = 0; s < FC_SLOTS; s++)
        {
            u32 skip, isDbl, done = 0, f = fc_reg(s);
            ppc_lbz(c, 5, (s32)s, 3);
            ppc_cmplwi(c, 0, 5, 0xFF);
            skip = ppc_bc_fwd(c, 12, 0, PPC_EQ);              // beq: unused
            ppc_rlwinm(c, 6, 5, 3, 24, 28);               // FGR * 8
            ppc_add(c, 6, 6, JR_CPU);
            ppc_andi_(c, 7, 5, FC_T_DBL);
            isDbl = ppc_bc_fwd(c, 4, 0, PPC_EQ);              // bne: a double
            if (st)
            {
                u32 noHiz;
                ppc_andi_(c, 7, 5, FC_T_HIZ);
                noHiz = ppc_bc_fwd(c, 12, 0, PPC_EQ);
                ppc_stw(c, 0, OFF_FGR(0), 6);
                ppc_patch_here(c, noHiz);
                ppc_stfs(c, f, OFF_FGR(0) + 4, 6);
            }
            else
                ppc_lfs(c, f, OFF_FGR(0) + 4, 6);
            done = ppc_b_fwd(c);
            ppc_patch_here(c, isDbl);
            if (st) ppc_stfd(c, f, OFF_FGR(0), 6);
            else ppc_lfd(c, f, OFF_FGR(0), 6);
            ppc_patch_here(c, done);
            ppc_patch_here(c, skip);
        }
        ppc_addi(c, 3, 3, FC_TABLE_WORDS * 4);
        ppc_mtlr(c, 3);
        ppc_blr(c);
    }
    if (g.c.overflow) H64_ERROR("[jit] shared runtime code too large");
#if defined(H64_JIT_ABI_ELFV1)
    {
        u64 *desc = (u64 *)j->mem;
        desc[0] = (u64)(uintptr_t)start;
        desc[1] = 0;
        desc[2] = 0;
        j->enter = (void (*)(H64System *, u32 *))(void *)desc;
    }
#else
    j->enter = (void (*)(H64System *, u32 *))(void *)start;
#endif
    j->memUsed = ((u32)(start - j->mem) + g.c.pos * 4 + 63) & ~63u;
    if (j->flushIcache) j->flushIcache(j->mem, j->memUsed);
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
        {
            u32 a, d;
            if (rd == 0) return 1;
            a = src_gpr(g, rt, 3);
            d = dst_gpr(g, rd, 3);
            if ((op & 63) == 0x00) { ppc_rlwinm(c, d, a, sa, 0, 31 - sa); ppc_extsw(c, d, d); }
            else if ((op & 63) == 0x02)
            {
                if (sa) { ppc_rlwinm(c, d, a, 32 - sa, sa, 31); ppc_extsw(c, d, d); }
                else ppc_extsw(c, d, a);
            }
            else { ppc_sradi(c, d, a, sa); ppc_extsw(c, d, d); }
            dst_done(g, d, rd);
            return 1;
        }
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
        {
            u32 a, b, d;
            if (rd == 0) return 1;
            a = src_gpr(g, rs, 3);
            b = src_gpr(g, rt, 4);
            d = dst_gpr(g, rd, 3);
            if ((op & 63) == 0x21 || (op & 63) == 0x2D) ppc_add(c, d, a, b);
            else ppc_subf(c, d, b, a);
            if ((op & 63) <= 0x23) ppc_extsw(c, d, d);
            dst_done(g, d, rd);
            return 1;
        }
        case 0x24: case 0x25: case 0x26: case 0x27:   // AND OR XOR NOR
        {
            u32 a, b, d;
            if (rd == 0) return 1;
            a = src_gpr(g, rs, 3);
            b = src_gpr(g, rt, 4);
            d = dst_gpr(g, rd, 3);
            if ((op & 63) == 0x24) ppc_and(c, d, a, b);
            else if ((op & 63) == 0x25) ppc_or(c, d, a, b);
            else if ((op & 63) == 0x26) ppc_xor(c, d, a, b);
            else ppc_nor(c, d, a, b);
            dst_done(g, d, rd);
            return 1;
        }
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
    {
        u32 a, d;
        if (rt == 0) return 1;
        a = src_gpr(g, rs, 3);   // never r0 (addi would read it as 0)
        d = dst_gpr(g, rt, 3);
        ppc_addi(c, d, a, simm);
        if (opc == 0x09) ppc_extsw(c, d, d);
        dst_done(g, d, rt);
        return 1;
    }
    case 0x0A: case 0x0B:   // SLTI SLTIU (the immediate is sign-extended for both)
        if (rt == 0) return 1;
        load_gpr(g, 3, rs);
        ppc_li(c, 4, simm);
        slt_result(g, opc == 0x0B, rt);
        return 1;
    case 0x0C: case 0x0D: case 0x0E:   // ANDI ORI XORI
    {
        u32 a, d;
        if (rt == 0) return 1;
        a = src_gpr(g, rs, 3);
        d = dst_gpr(g, rt, 3);
        if (opc == 0x0C) ppc_andi_(c, d, a, uimm);
        else if (opc == 0x0D) ppc_ori(c, d, a, uimm);
        else ppc_xori(c, d, a, uimm);
        dst_done(g, d, rt);
        return 1;
    }
    case 0x0F:   // LUI
    {
        u32 d;
        if (rt == 0) return 1;
        d = dst_gpr(g, rt, 3);
        ppc_lis(c, d, simm);
        dst_done(g, d, rt);
        return 1;
    }
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
    u32 alt = 0, done = 0, pass, n = (ft & 1) ? 2 : 1, first = 0;
    if (g->fpuKnown && n == 2)
    {
        n = 1;                        // FR known: one side only
        first = g->fpuFr ? 0 : 1;
    }
    for (pass = first; pass < first + n; pass++)
    {
        // pass 0: FR = 1 (or an even register); pass 1: FR = 0 with an odd register.
        s32 off;
        if (pass == 0 && n == 2)
        {
            ppc_cmpdi(c, 0, 7, 0);
            alt = ppc_bc_fwd(c, 12, 0, PPC_EQ);   // beq: FR = 0
        }
        if (pass == 1 && n == 2) ppc_patch_here(c, alt);
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

// The first COP1 instruction of a block checks Status.CU1 and FR against the
// values when the block was compiled; the following ones rely on them (Status
// only changes through COP0 instructions, which end blocks). DK64's matrix
// code spent most of its time re-reading Status and FCR31 for every operation.
// A failed guard sends the instruction to the interpreter and leaves the block.
// Returns 0 when nothing is known (COP1 unusable at compile time): the caller
// then checks at run time as before.
static int fpu_guard(Gen *g, u32 *slow, u32 *nSlow)
{
    H64PpcCode *c = &g->c;
    u32 sr = (u32)g->sys->cpu.cop0[CP0_STATUS];
    if (g->fpuKnown) return 1;
    if (!(sr & 0x20000000u) || g->sys->jit->noFpuGuard) return 0;
    ppc_ld(c, 5, OFF_COP0(CP0_STATUS), JR_CPU);
    ppc_rlwinm(c, 6, 5, 16, 16, 31);          // Status >> 16
    ppc_andi_(c, 6, 6, 0x2400);               // CU1 | FR
    ppc_cmplwi(c, 0, 6, (sr & 0x24000000u) >> 16);
    slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    g->tailExit = 1;
    g->fpuKnown = 1;
    g->fpuFr = (sr >> 26) & 1;
    return 1;
}

// FCR31's rounding mode is nearest (the host's): checked once until a CTC1
// or an instruction run by the interpreter.
static void rm_guard(Gen *g, u32 *slow, u32 *nSlow)
{
    H64PpcCode *c = &g->c;
    if (g->rmNearest) return;
    ppc_andi_(c, 6, 5, 3);   // r5 = FCR31
    slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    if (!g->sys->jit->noFpuGuard && !((u32)g->sys->cpu.fcr31 & 3))
    {
        g->tailExit = 1;
        g->rmNearest = 1;
    }
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
    if (fpu && !fpu_guard(g, slow, &nSlow))
    {
        // COP1 must be usable (else the slow path raises the exception). r7
        // keeps Status.FR for the register selection below.
        ppc_ld(c, 5, OFF_COP0(CP0_STATUS), JR_CPU);
        ppc_andis_(c, 7, 5, 0x0400);     // FR
        ppc_andis_(c, 5, 5, 0x2000);     // CU1
        slow[nSlow++] = ppc_bc_fwd(c, 12, 0, PPC_EQ);   // beq: CU1 clear
    }
    ppc_addi(c, 3, src_gpr(g, rs, 3), simm);
    // Shared check (h64_jit_emit_runtime): r4 = physical address, cr0.eq = fast path.
    ppc_branch_to(c, g->sys->jit->rtCheck[store][size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3], 1);
    slow[nSlow++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    if (fpu && fc_on(g))
    {
        // Through the FPR cache (FR known). The FGR: LWC1/SWC1 use the low
        // word of fgr[rt] unless FR = 0 and rt is odd (the high word of
        // fgr[rt - 1]: through memory); LDC1/SDC1 the whole fgr[rt] (FR = 1)
        // or fgr[rt & ~1].
        RegCache *r = &g->rc;
        int direct = size == 8 || g->fpuFr || !(rt & 1);
        u32 m = size == 8 && !g->fpuFr ? (rt & ~1u) : rt, bit;
        if (!direct)
        {
            m = rt - 1;
            if (store) fc_sync(g, m);
            else
            {
                fc_flush(g, m);   // a partial write in memory
                ppc_lwzx(c, 5, JR_RDRAM, 4);
            }
            fpu_access(g, rt, size, store);
            if (store) ppc_stwx(c, 5, JR_RDRAM, 4);
            *nSlowOut = nSlow;
            return 1;
        }
        bit = 1u << m;
        if (store)
        {
            int dbl = size == 8;
            if (r->fhost[m] >= 0 && ((r->fdbl & bit) != 0) == dbl)
            {
                u32 f = fc_src(g, m, dbl);
                if (dbl) ppc_stfdx(c, f, JR_RDRAM, 4);
                else ppc_stfsx(c, f, JR_RDRAM, 4);
            }
            else
            {
                // Not cached, or in the other format: the value from H64Cpu.
                fc_sync(g, m);
                fpu_access(g, rt, size, 1);
                if (dbl) ppc_stdx(c, 5, JR_RDRAM, 4);
                else ppc_stwx(c, 5, JR_RDRAM, 4);
            }
        }
        else if (size == 8)
            ppc_lfdx(c, fc_dst(g, m, 1, 0), JR_RDRAM, 4);
        else
        {
            // LWC1 replaces the low word only: what a dirty entry holds of the
            // high word goes to H64Cpu first.
            if (r->fhost[m] >= 0 && (r->fdirty & bit))
            {
                if (r->fdbl & bit) fc_sync(g, m);
                else if (r->fhiz & bit)
                {
                    ppc_li(c, 0, 0);
                    ppc_stw(c, 0, OFF_FGR(m), JR_CPU);
                }
            }
            ppc_lfsx(c, fc_dst(g, m, 0, 0), JR_RDRAM, 4);
        }
        *nSlowOut = nSlow;
        return 1;
    }
    if (store)
    {
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
// The slow paths are emitted after the block's body (emit_cold_tails), so the
// fast paths stay contiguous in the instruction cache; each one branches back
// to the instruction after its fast path.
struct ColdTail
{
    u32 slow[16], nSlow;
    u32 pc, pend, back;
    u32 veneer;           // hot word: b <the tail in the cold region>
    u32 *coldStart;
    int exitAfter;        // leave the block after the interpreter ran the instruction (a guard failed)
    u32 ftouch;           // FGRs the instruction may read or write
    RegCache pre, post;
};

// Every FGR a COP1 instruction or a COP1 load/store may access, whatever FR
// is (a superset): fs, ft, fd and their even registers.
static u32 fp_touch(u32 op)
{
    u32 opc = op >> 26, ft = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31;
    if (opc == 0x31 || opc == 0x35 || opc == 0x39 || opc == 0x3D) return (1u << ft) | (1u << (ft & ~1u));
    if (opc == 0x11)
        return (1u << ft) | (1u << (ft & ~1u)) | (1u << fs) | (1u << (fs & ~1u)) | (1u << fd) | (1u << (fd & ~1u));
    return 0;
}
static ColdTail s_tails[H64_JIT_MAX_INSNS + 2];   // one block is compiled at a time
static u32 s_nTails;

static void emit_slow_tail(Gen *g, u32 pc, const u32 *slow, u32 nSlow)
{
    ColdTail *t;
    u32 i;
    if (s_nTails >= H64_JIT_MAX_INSNS + 2) { g->c.overflow = 1; return; }
    t = &s_tails[s_nTails++];
    for (i = 0; i < nSlow && i < 16; i++) t->slow[i] = slow[i];
    t->nSlow = nSlow < 16 ? nSlow : 16;
    t->pc = pc;
    t->pend = g->pending;
    t->back = g->c.pos;
    t->pre = g->pre;
    t->post = g->rc;
    t->exitAfter = g->tailExit;
    t->ftouch = g->ftouch | fp_touch(h64_load_be32(g->sys->rdram + g->paddr0 + (pc - g->pc0)));
    g->tailExit = 0;
    g->pending++;
}

// Hot side: the slow branches of each tail go to a one-word veneer at the end
// of the block (conditional branches reach +-32 KB), filled in later.
static void emit_cold_veneers(Gen *g)
{
    H64PpcCode *c = &g->c;
    u32 k, i;
    for (k = 0; k < s_nTails; k++)
    {
        ColdTail *t = &s_tails[k];
        for (i = 0; i < t->nSlow; i++) ppc_patch_here(c, t->slow[i]);
        t->veneer = c->pos;
        ppc_put(c, 0x48000000u);
    }
}

// Slow paths: the FGRs of state r in `mask` to H64Cpu (store) or back from
// it: inline when that is not longer, else a call to rtFcStore/rtFcLoad
// followed by their table (one byte per slot, in memory order).
static void fc_slow_call(Gen *g, const u32 *routine, const RegCache *r, u32 mask, int store)
{
    H64PpcCode *c = &g->c;
    u8 table[FC_TABLE_WORDS * 4];
    u32 m, words = 0, zeroWord = 0, i;
    memset(table, 0xFF, sizeof(table));
    for (m = 0; m < 32; m++)
        if (r->fhost[m] >= 0 && (mask & (1u << m)))
        {
            u32 bit = 1u << m;
            table[(u32)r->fhost[m]] = (u8)(m | ((r->fdbl & bit) ? FC_T_DBL : 0) | ((r->fhiz & bit) ? FC_T_HIZ : 0));
            words++;
            if (store && !(r->fdbl & bit) && (r->fhiz & bit)) { words++; zeroWord = 1; }
        }
    if (!words) return;
    if (words + zeroWord <= 1 + FC_TABLE_WORDS)
    {
        int zero = 0;
        for (m = 0; m < 32; m++)
            if (r->fhost[m] >= 0 && (mask & (1u << m)))
            {
                if (store) fc_store(g, r, m, &zero);
                else fc_load(g, r, m);
            }
        return;
    }
    ppc_branch_to(c, routine, 1);
    for (i = 0; i < FC_TABLE_WORDS; i++)
    {
        if (c->pos >= c->cap) { c->overflow = 1; return; }
        memcpy(c->buf + c->pos, table + 4 * i, 4);   // bytes in memory order on any host
        c->pos++;
    }
}

// Cold side (g->c is the cold buffer here, hot the block's buffer).
static void emit_cold_tails(Gen *g, H64PpcCode *hot)
{
    H64PpcCode *c = &g->c;
    u32 k;
    for (k = 0; k < s_nTails; k++)
    {
        ColdTail *t = &s_tails[k];
        u32 m, reload = 0;
        t->coldStart = c->buf + c->pos;
        if (g->cacheOn)
        {
            rc_writeback_gpr(g, &t->pre);
            fc_slow_call(g, g->sys->jit->rtFcStore, &t->pre, t->pre.fdirty, 1);
        }
        ppc_li32u(c, 4, t->pc);
        ppc_li32u(c, 5, h64_load_be32(g->sys->rdram + g->paddr0 + (t->pc - g->pc0)));
        ppc_li(c, 9, (s32)t->pend);
        ppc_li(c, 10, (s32)(t->pend * g->cpi));
        ppc_li(c, 11, (s32)g->cpi);
        ppc_branch_to(c, g->sys->jit->rtSlow, 1);
        if (t->exitAfter)
        {
            // What the rest of the block assumed about COP1 does not hold:
            // leave, with the counters rtSlow took back counted again.
            add_exit(g, ppc_bc_fwd(c, 4, 0, PPC_EQ));
            add_to(g, OFF_CYCLES, (s32)((t->pend + 1) * g->cpi));
            add_to(g, OFF_INSNS, (s32)(t->pend + 1));
            add_exit(g, ppc_b_fwd(c));
            continue;
        }
        add_exit(g, ppc_bc_fwd(c, 4, 0, PPC_EQ));   // bne exit
        if (g->cacheOn)
        {
            rc_reload(g, &t->post);
            // FGRs whose host FPR may not hold the value: in a volatile FPR
            // (the call clobbers f0-f13), accessed by the instruction, or
            // mapped or changed in format during it. The others kept theirs.
            for (m = 0; m < 32; m++)
            {
                s8 sl = t->post.fhost[m];
                if (sl < 0) continue;
                if (fc_reg((u32)sl) < 14 || (t->ftouch & (1u << m)) || t->pre.fhost[m] != sl ||
                    ((t->pre.fdbl ^ t->post.fdbl) & (1u << m)))
                    reload |= 1u << m;
            }
            fc_slow_call(g, g->sys->jit->rtFcLoad, &t->post, reload, 0);
        }
        ppc_branch_to(c, hot->buf + t->back, 0);   // back to the fast path's end
    }
}

static void fill_cold_veneers(H64PpcCode *hot)
{
    u32 k;
    for (k = 0; k < s_nTails; k++)
    {
        const ColdTail *t = &s_tails[k];
        s32 off = (s32)((const u8 *)t->coldStart - (const u8 *)(hot->buf + t->veneer));
        if (t->veneer < hot->cap) hot->buf[t->veneer] = 0x48000000u | ((u32)off & 0x03FFFFFCu);
    }
    s_nTails = 0;
}

static int emit_mem(Gen *g, u32 op, u32 pc)
{
    u32 slow[16], nSlow = 0;
    if (!emit_mem_fast(g, op, slow, &nSlow)) return 0;
    emit_slow_tail(g, pc, slow, nSlow);
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
    u32 slow[16], nSlow = 0;
    if (!emit_ovf_fast(g, op, slow, &nSlow)) return 0;
    emit_slow_tail(g, pc, slow, nSlow);
    return 1;
}

// ---- Native FPU ----
// COP1 arithmetic (ADD SUB MUL DIV), compares, MOV, ABS, NEG, CVT.D.S, CVT.S.D,
// TRUNC.W, CVT.W and CVT.S/D.W run on the host FPU when the result is certain
// to be the interpreter's: operands normal, zero or infinite, guest rounding
// mode nearest (the host's), no host flag but inexact (FPSCR VX, OX, UX, ZX
// clear; FI, the non-sticky inexact bit of the last operation, gives the MIPS
// Inexact cause) and a normal, zero or infinite result. The checks are shared
// routines (rtFpCheck, rtFpFinish in h64_jit_emit_runtime). Anything else
// (NaN, denormal, overflow, an enabled Inexact trap...) goes to the slow path,
// the interpreter, before anything is written. Host FPU flags must be
// readable (not in Xenia: h64_fenv_reliable).
static void fp_call(Gen *g, const u32 *routine, u32 *slow, u32 *nSlow)
{
    ppc_branch_to(&g->c, routine, 1);
    slow[(*nSlow)++] = ppc_bc_fwd(&g->c, 4, 0, PPC_EQ);   // bne slow
}

static void fp_store32_zext(Gen *g, u32 fd)
{
    ppc_li(&g->c, 8, 0);
    ppc_stw(&g->c, 8, OFF_FGR(fd), JR_CPU);
}

// The result in f1 goes to FGR fd: the cached FPR, or H64Cpu (a single
// zeroes the high word).
static void fp_result(Gen *g, u32 fd, int dbl)
{
    if (fc_on(g))
    {
        ppc_fmr(&g->c, fc_dst(g, fd, dbl, 1), 1);
        return;
    }
    if (dbl) ppc_stfd(&g->c, 1, OFF_FGR(fd), JR_CPU);
    else
    {
        fp_store32_zext(g, fd);
        ppc_stfs(&g->c, 1, OFF_FGR(fd) + 4, JR_CPU);
    }
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
    u32 a = 1, b = 2;
    int fast, fc;
    if ((op >> 26) != 0x11) return 0;
    if (!move)
    {
        if (!g->fpu || (fmt != 0x10 && fmt != 0x11 && !fromW)) return 0;
        if (!arith && !cmp && funct != 0x06 && funct != 0x05 && funct != 0x07 && !cvtd && !cvts && !toint && !fromW) return 0;
    }

    // COP1 usable; an odd fs register needs FR = 1 (FR = 0 would use fs - 1).
    {
        u32 sr = (u32)g->sys->cpu.cop0[CP0_STATUS];
        int guarded = g->fpuKnown || ((sr & 0x20000000u) && !g->sys->jit->noFpuGuard);
        u32 fr = g->fpuKnown ? g->fpuFr : (sr >> 26) & 1;
        if (guarded && (fs & 1) && fmt != 2 && fmt != 6 && !move && !fr) return 0;   // the interpreter
    }
    if (!fpu_guard(g, slow, nSlow))
    {
        ppc_ld(c, 5, OFF_COP0(CP0_STATUS), JR_CPU);
        ppc_andis_(c, 6, 5, 0x2000);
        slow[(*nSlow)++] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        if ((fs & 1) && fmt != 2 && fmt != 6 && !move)
        {
            ppc_andis_(c, 6, 5, 0x0400);
            slow[(*nSlow)++] = ppc_bc_fwd(c, 12, 0, PPC_EQ);
        }
    }
    if (move)
    {
        // With FR = 0 an odd register is the high word of the even one (32-bit
        // moves) or the even register itself (64-bit moves).
        u32 pass, n = ((fs & 1) && fmt != 2 && fmt != 6) ? 2 : 1, alt = 0, done = 0, first = 0;
        if (g->fpuKnown && n == 2)
        {
            n = 1;
            first = g->fpuFr ? 0 : 1;
        }
        if (fmt == 0 || fmt == 1 || fmt == 4 || fmt == 5)
        {
            // The GPR is read once, before the FR split: a register cached on
            // one side only would hold garbage on the other.
            if (fmt == 4 || fmt == 5) load_gpr(g, 3, ft);
            if (fc_on(g))
            {
                // FR known (n = 1): these go through H64Cpu.
                u32 m = first ? fs - 1 : fs;
                if (fmt <= 1) fc_sync(g, m);
                else if (fmt == 4) fc_flush(g, m);   // the low or high word only
                else fc_drop(g, m);
            }
            for (pass = first; pass < first + n; pass++)
            {
                s32 off32 = pass ? OFF_FGR(fs - 1) : OFF_FGR(fs) + 4, off64 = pass ? OFF_FGR(fs - 1) : OFF_FGR(fs);
                if (pass == 0 && n == 2)
                {
                    ppc_andis_(c, 6, 5, 0x0400);   // FR
                    alt = ppc_bc_fwd(c, 12, 0, PPC_EQ);
                }
                if (pass == 1 && n == 2) ppc_patch_here(c, alt);
                switch (fmt)
                {
                case 0: ppc_lwa(c, 3, off32, JR_CPU); store_gpr(g, 3, ft); break;    // MFC1
                case 1: ppc_ld(c, 3, off64, JR_CPU); store_gpr(g, 3, ft); break;     // DMFC1
                case 4: ppc_stw(c, 3, off32, JR_CPU); break;     // MTC1
                default: ppc_std(c, 3, off64, JR_CPU); break;    // DMTC1
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
            g->rmNearest = 0;
            break;
        }
        return 1;
    }
    fc = fc_on(g);
    if (funct == 0x06)   // MOV: the whole register
    {
        if (fc)
        {
            // Cached as a double, or as a single with a zero high word, or
            // MOV.D of an uncached register: a copy between host FPRs.
            RegCache *r = &g->rc;
            u32 bit = 1u << fs;
            int mapped = r->fhost[fs] >= 0;
            if ((mapped && ((r->fdbl & bit) || (r->fhiz & bit))) || (!mapped && dbl))
            {
                int asDbl = mapped ? (r->fdbl & bit) != 0 : 1;
                u32 src = fc_src(g, fs, asDbl), dst = fc_dst(g, fd, asDbl, 1);
                if (dst != src) ppc_fmr(c, dst, src);
                return 1;
            }
            fc_sync(g, fs);
            fc_drop(g, fd);
        }
        ppc_ld(c, 5, OFF_FGR(fs), JR_CPU);
        ppc_std(c, 5, OFF_FGR(fd), JR_CPU);
        return 1;
    }

    // fastFpu (mupen64plus's way): the operation on the host FPU as it is,
    // without the operand and result checks or FCR31's cause and flag bits
    // (each of those read FCR31 back right after storing it, read the FPSCR
    // with mffs and called two shared routines: ~100 cycles per operation).
    // Compares still write C; rounding modes other than nearest still go to
    // the interpreter.
    fast = g->sys->jit->fastFpu;
    if (!fast || cmp || !g->rmNearest) ppc_lwz(c, 5, OFF_FCR31, JR_CPU);
    if (fromW)
    {
        // CVT.S.W, CVT.D.W: the integer through memory (fcfid converts a
        // doubleword), exact as a double; rounded once to single.
        if (fc) fc_sync(g, fs);
        ppc_lwa(c, 6, OFF_FGR(fs) + 4, JR_CPU);
        ppc_std(c, 6, OFF_JITSCRATCH, JR_CPU);
        ppc_lfd(c, 1, OFF_JITSCRATCH, JR_CPU);
        ppc_fcfid(c, 1, 1);
        if (funct == 0x21)
        {
            if (!fast)
            {
                ppc_rlwinm(c, 5, 5, 0, 20, 13);
                ppc_stw(c, 5, OFF_FCR31, JR_CPU);
            }
            fp_result(g, fd, 1);
            return 1;
        }
        rm_guard(g, slow, nSlow);   // rounding mode nearest
        ppc_frsp(c, 1, 1);
        if (!fast) fp_call(g, g->sys->jit->rtFpFinish[0], slow, nSlow);
        fp_result(g, fd, 0);
        return 1;
    }
    if (arith || cvts) rm_guard(g, slow, nSlow);   // rounding mode nearest
    if (funct == 0x24 && !g->rmNearest)
    {
        // CVT.W: nearest or towards zero (IDO's (int) casts set RM = 1 around it); cr0.eq = nearest.
        ppc_andi_(c, 6, 5, 2);
        slow[(*nSlow)++] = ppc_bc_fwd(c, 4, 0, PPC_EQ);
    }
    // The operands: cached host FPRs, or f1 and f2 loaded from H64Cpu.
    if (fc)
    {
        a = fc_src(g, fs, dbl);
        if (arith || cmp) b = fc_src(g, ft, dbl);
    }
    else
    {
        s32 offS = dbl ? OFF_FGR(fs) : OFF_FGR(fs) + 4, offT = dbl ? OFF_FGR(ft) : OFF_FGR(ft) + 4;
        a = 1;
        if (dbl) ppc_lfd(c, 1, offS, JR_CPU);
        else ppc_lfs(c, 1, offS, JR_CPU);
        if (arith || cmp)
        {
            b = 2;
            if (dbl) ppc_lfd(c, 2, offT, JR_CPU);
            else ppc_lfs(c, 2, offT, JR_CPU);
        }
    }

    if (cmp)
    {
        // No NaN: no exception, cause cleared, C = the condition.
        u32 cond = funct & 15, set[2], nSet = 0, over, i;
        ppc_fcmpu(c, 0, a, b);
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

    // Operand checks (shared routine, f1 and f2): normal, zero or infinite.
    if (!fast)
    {
        if (a != 1) ppc_fmr(c, 1, a);
        if (arith && b != 2) ppc_fmr(c, 2, b);
        fp_call(g, g->sys->jit->rtFpCheck[dbl][arith ? 1 : 0], slow, nSlow);
    }
    if (funct == 0x05 || funct == 0x07)
    {
        // ABS, NEG: no exception for these operands; the cause is cleared.
        u32 d = fc ? fc_dst(g, fd, dbl, 1) : 1;
        if (funct == 0x05) ppc_fabs(c, d, a);
        else ppc_fneg(c, d, a);
        if (!fast)
        {
            ppc_rlwinm(c, 5, 5, 0, 20, 13);
            ppc_stw(c, 5, OFF_FCR31, JR_CPU);
        }
        if (!fc) fp_result(g, fd, dbl);
        return 1;
    }
    if (cvtd)
    {
        // Exact: a normal single is a normal double, no flag.
        u32 d = fc ? fc_dst(g, fd, 1, 1) : 1;
        if (!fast)
        {
            ppc_rlwinm(c, 5, 5, 0, 20, 13);
            ppc_stw(c, 5, OFF_FCR31, JR_CPU);
        }
        if (d != a) ppc_fmr(c, d, a);
        if (!fc) fp_result(g, fd, 1);
        return 1;
    }
    if (toint)
    {
        if (funct == 0x0D) ppc_fctiwz(c, 1, a);
        else if (g->rmNearest) ppc_fctiw(c, 1, a);
        else
        {
            u32 nearest, join;
            ppc_andi_(c, 6, 5, 1);   // cr0.eq = nearest
            nearest = ppc_bc_fwd(c, 12, 0, PPC_EQ);
            ppc_fctiwz(c, 1, a);
            join = ppc_b_fwd(c);
            ppc_patch_here(c, nearest);
            ppc_fctiw(c, 1, a);
            ppc_patch_here(c, join);
        }
        if (!fast) fp_call(g, g->sys->jit->rtFpFinish[0], slow, nSlow);   // NaN, infinite or out of range: VXCVI
        if (fc) fc_drop(g, fd);   // an integer: through H64Cpu
        fp_store32_zext(g, fd);
        ppc_addi(c, 8, JR_CPU, OFF_FGR(fd) + 4);
        ppc_stfiwx(c, 1, 0, 8);
        return 1;
    }
    {
        // Arithmetic or CVT.S.D; with fastFpu nothing follows the operation,
        // which then writes the cached destination directly.
        int dblOut = dbl && !cvts;
        u32 d = (fc && fast) ? fc_dst(g, fd, dblOut, 1) : 1;
        if (arith)
        {
            switch (funct)
            {
            case 0: if (dbl) ppc_fadd(c, d, a, b); else ppc_fadds(c, d, a, b); break;
            case 1: if (dbl) ppc_fsub(c, d, a, b); else ppc_fsubs(c, d, a, b); break;
            case 2: if (dbl) ppc_fmul(c, d, a, b); else ppc_fmuls(c, d, a, b); break;
            default: if (dbl) ppc_fdiv(c, d, a, b); else ppc_fdivs(c, d, a, b); break;
            }
        }
        else
            ppc_frsp(c, d, a);   // CVT.S.D
        // Flags, a denormal result, Inexact and FCR31 (shared routine).
        if (!fast) fp_call(g, g->sys->jit->rtFpFinish[dblOut ? 2 : 1], slow, nSlow);
        if (d == 1) fp_result(g, fd, dblOut);
    }
    return 1;
}

static int emit_fpu(Gen *g, u32 op, u32 pc)
{
    u32 slow[16], nSlow = 0;
    if (!emit_fpu_fast(g, op, slow, &nSlow)) return 0;
    emit_slow_tail(g, pc, slow, nSlow);
    return 1;
}

// ---- Branches ----
// Computes R_COND (1: taken) and R_TARGET for a native branch; writes the link.
// Returns 0 when the branch is not handled natively (BC1x).
// The exits after a native branch with a fixed target and its slot: compact
// linked exits per outcome; J/JAL and BEQ rs,rs only have the taken one.
static void branch_exit_n(Gen *g, u32 op, u32 pending, u64 target, u32 fallthrough)
{
    u32 opc = op >> 26, notTaken;
    int always = opc == 0x02 || opc == 0x03 || (opc == 0x04 && ((op >> 21) & 31) == ((op >> 16) & 31));
    if (!g->native)
    {
        exit_block(g);
        return;
    }
    if (always)
    {
        link_exit_n(g, pending, (u32)target);
        return;
    }
    ppc_cmpdi(&g->c, 0, R_COND, 0);
    notTaken = ppc_bc_fwd(&g->c, 12, 0, PPC_EQ);
    link_exit_n(g, pending, (u32)target);
    ppc_patch_here(&g->c, notTaken);
    link_exit_n(g, pending, fallthrough);
}

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
    begin_insn(g);
    g->bc1Pre = g->rc;
    emit_branch_body(g, op, pc, ds);
    if (g->hasBc1Slow)
    {
        ppc_patch_here(&g->c, g->bc1Slow);
        g->rc = g->bc1Pre;
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
    RegCache slotPre;
    emit_branch_head(g, op, pc, &likely, &dynamicTarget, &target);
    g->pending++;   // the branch itself
    if (likely)
    {
        // Not taken: the delay slot is skipped, the block ends at pc + 8.
        u32 takenAt;
        ppc_cmpdi(c, 0, R_COND, 0);
        takenAt = ppc_bc_fwd(c, 4, 0, PPC_EQ);   // taken: go on with the slot
        link_exit_n(g, g->pending, pc + 8);
        ppc_patch_here(c, takenAt);
    }
    // The delay slot.
    begin_insn(g);
    slotPre = g->rc;
    if (g->native && !is_branch(ds) && !ends_block(ds) && (emit_alu(g, ds) ? (g->pending++, 1) : 0))
    {
        if (!dynamicTarget && !g->sys->jit->noLink && !g->sys->jit->fullExits)
        {
            branch_exit_n(g, op, g->pending, target, dsPc + 4);
            g->pending = 0;
            return;
        }
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
            // The compact exits diverged here (Conker, lockstep under QEMU,
            // cause not found yet): the full exits after a load/store, FPU or
            // overflow-checked slot.
            add_to(g, OFF_CYCLES, (s32)((pend + 1) * g->cpi));
            add_to(g, OFF_INSNS, (s32)(pend + 1));
            store_branch_pc(g, dynamicTarget, target, dsPc + 4);
            ppc_li(c, 3, 0);
            ppc_stw(c, 3, OFF_BRANCH, JR_CPU);
            branch_exit(g, dynamicTarget, target, dsPc + 4);
            for (i = 0; i < nSlow; i++) ppc_patch_here(c, slow[i]);
            g->pending = pend;
            g->rc = slotPre;   // the slow paths left before the slot changed anything
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
    u32 ops[H64_JIT_MAX_INSNS], n = 0, i, bodyAt, coldWords = 0;
    int coldOverflow = 0;
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

    if (j->blockCount >= j->blockCap || j->memUsed + MAX_BLOCK_BYTES + 64 > j->coldBase ||
        j->coldUsed + MAX_COLD_BYTES + 64 > j->memSize)
    {
        H64_INFO("[jit] code cache full (%u blocks, hot %u KB, cold %u KB): flushed", j->blockCount, j->memUsed >> 10,
                 (j->coldUsed - j->coldBase) >> 10);
        h64_jit_reset(sys);
    }

    j->memUsed = (j->memUsed + 15) & ~15u;
    start = j->mem + j->memUsed;
    memset(&g, 0, sizeof(g));
    g.sys = sys;
    g.c.buf = (u32 *)start;
    g.c.cap = MAX_BLOCK_BYTES / 4;
    g.pc0 = pc;
    g.paddr0 = paddr;
    g.cpi = sys->cpu.cpi ? sys->cpu.cpi : 1;
    g.native = h64_jit_kernel_mode(&sys->cpu) && !j->noNative;
    g.fpu = g.native && !j->noFpu && h64_fenv_reliable();
    g.cacheOn = g.native && !j->noRegCache;
    rc_reset(&g.rc);
    g.pre = g.rc;
    sys->cpu.jitFpMin[0] = ldexp(1.0, -126);
    sys->cpu.jitFpMin[1] = ldexp(1.0, -1022);

    s_nTails = 0;
    s_nLinkTails = 0;
    bodyAt = 0;   // entered through j->enter (the shared prologue)
    for (i = 0; i < n; i++)
    {
        u32 ipc = pc + i * 4, op = ops[i];
        if (!(endsWithBranch && i == n - 2)) begin_insn(&g);
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
        if (ends_block(ops[n - 1]))
        {
            sync_to(&g, g.pending, pc + n * 4);
            exit_block(&g);   // COP0: the mode may have changed
        }
        else
            link_exit_n(&g, g.pending, pc + n * 4);
    }
    // The slow paths go to the cold region (they would dilute the hot code in
    // the caches): veneers here, then the shared exit.
    emit_cold_veneers(&g);
    for (i = 0; i < g.nExits; i++) ppc_patch_here(&g.c, g.exits[i]);
    ppc_branch_to(&g.c, j->rtExit, 0);   // the shared epilogue
    j->stats.hotBytes += g.c.pos * 4;
    {
        H64PpcCode hot = g.c;
        g.c.buf = (u32 *)(j->mem + j->coldUsed);
        g.c.pos = 0;
        g.c.cap = MAX_COLD_BYTES / 4;
        g.c.overflow = 0;
        g.nExits = 0;
        emit_cold_tails(&g, &hot);
        emit_cold_links(&g, &hot);
        for (i = 0; i < g.nExits; i++) ppc_patch_here(&g.c, g.exits[i]);
        ppc_branch_to(&g.c, j->rtExit, 0);
        fill_cold_veneers(&hot);
        coldWords = g.c.pos;
        coldOverflow = g.c.overflow;
        g.c = hot;
    }
    if (g.c.overflow || coldOverflow)
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
    b->fn = 0;
    if (j->dumpFile)
    {
        // vpc, MIPS instruction count, hot PowerPC words, the MIPS words, the hot code (host byte order).
        u32 hdr[3], k;
        hdr[0] = pc; hdr[1] = n; hdr[2] = g.c.pos;
        fwrite(hdr, 4, 3, (FILE *)j->dumpFile);
        for (k = 0; k < n; k++) { u32 w = ops[k]; fwrite(&w, 4, 1, (FILE *)j->dumpFile); }
        fwrite(start, 4, g.c.pos, (FILE *)j->dumpFile);
    }
    j->memUsed = (u32)((start - j->mem) + g.c.pos * 4);
    if (j->flushIcache)
    {
        j->flushIcache(start, g.c.pos * 4);
        if (coldWords) j->flushIcache(j->mem + j->coldUsed, coldWords * 4);
    }
    j->coldUsed = (j->coldUsed + coldWords * 4 + 15) & ~15u;

    b->hashNext = j->hash[(pc >> 2) & 8191];
    j->hash[(pc >> 2) & 8191] = b;
    b->pageNext = j->pageHead[paddr >> 12];
    j->pageHead[paddr >> 12] = b;
    h64_jit_code_map(j, b, 1);
    j->stats.blocksCompiled++;
    j->stats.codeBytes += (g.c.pos + coldWords) * 4;
    H64_DEBUG("[jit] block %08X (phys %06X): %u instructions, %u bytes, first %08X", pc, paddr, n, g.c.pos * 4, ops[0]);
    return b;
}
