// Harissa64 V2 - recompiler code generation.
//
// Block layout (64-bit PowerPC):
//   prologue: stdu/stwu r1,-FRAME(r1); mflr r0; save r0 and r28..r31 in
//             the top of the frame; r31 = sys (first argument)
//   body:     one sequence per MIPS instruction
//   exit:     restore and blr
// The frame keeps the 112-byte ELFv1 header and parameter area at its
// bottom, where called C functions save their link register and TOC on
// ppc64 Linux; Xbox functions save theirs below their own stack pointer.
#include "h64_jit.h"
#include "h64_jit_internal.h"
#include "h64_ppc_emit.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"

#define FRAME 176
#define MAX_BLOCK_BYTES (H64_JIT_MAX_INSNS * 96 + 512)

// ---- Helpers called from generated code ----

// Runs the instruction at cpu->pc with the reference interpreter. Returns
// non-zero when the block must stop: an exception, a pc other than the
// next one in the block (taken branch, nullified delay slot, end of the
// delay slot), or one of h64_jit_should_exit's reasons.
static int helper_interp(H64System *sys, u32 expectedNext)
{
    H64Cpu *cpu = &sys->cpu;
    h64_cpu_step(sys);
    if (cpu->exceptionRaised || cpu->pc != (u64)(s64)(s32)expectedNext) return 1;
    return h64_jit_should_exit(sys);
}

// ---- Calls ----
static u64 fn_addr(int (*f)(H64System *, u32)) { return (u64)(uintptr_t)f; }

static void emit_load_callee(H64PpcCode *c, u32 reg, u64 addr)
{
#if defined(H64_JIT_ABI_XBOX)
    ppc_li32u(c, reg, (u32)addr);
#else
    ppc_li64(c, reg, addr);
#endif
}

// Calls the function whose address (or ELFv1 descriptor) is in `reg`.
static void emit_call_reg(H64PpcCode *c, u32 reg)
{
#if defined(H64_JIT_ABI_ELFV1)
    ppc_ld(c, 0, 0, reg);
    ppc_ld(c, 2, 8, reg);
    ppc_mtctr(c, 0);
#else
    ppc_mtctr(c, reg);
#endif
    ppc_bctrl(c);
}

static void emit_prologue(H64PpcCode *c)
{
#if defined(H64_JIT_ABI_XBOX)
    ppc_stwu(c, 1, -FRAME, 1);   // 32-bit back chain, as the XDK compiler does
#else
    ppc_stdu(c, 1, -FRAME, 1);
#endif
    ppc_mflr(c, 0);
    ppc_std(c, 0, FRAME - 8, 1);
    ppc_std(c, JR_SYS, FRAME - 16, 1);
    ppc_std(c, JR_CPU, FRAME - 24, 1);
    ppc_std(c, JR_RDRAM, FRAME - 32, 1);
    ppc_std(c, JR_TMP, FRAME - 40, 1);
    ppc_mr(c, JR_SYS, 3);
}

static void emit_epilogue(H64PpcCode *c)
{
    ppc_ld(c, 0, FRAME - 8, 1);
    ppc_mtlr(c, 0);
    ppc_ld(c, JR_SYS, FRAME - 16, 1);
    ppc_ld(c, JR_CPU, FRAME - 24, 1);
    ppc_ld(c, JR_RDRAM, FRAME - 32, 1);
    ppc_ld(c, JR_TMP, FRAME - 40, 1);
    ppc_addi(c, 1, 1, FRAME);
    ppc_blr(c);
}

// ---- Block formation ----
// Instructions after which control may go elsewhere: branches and jumps
// (the block then ends after their delay slot).
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

H64JitBlock *h64_jit_compile(H64System *sys, u32 pc, u32 paddr)
{
    H64Jit *j = sys->jit;
    H64JitBlock *b;
    H64PpcCode c;
    u32 ops[H64_JIT_MAX_INSNS], n = 0, i, exitFix[H64_JIT_MAX_INSNS], nFix = 0;
    u32 pageEnd = (paddr | 0xFFF) + 1;
    u8 *start;

    // Formation: stop at the page end, after a branch's delay slot, or at the size limit.
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
            break;
        }
        ops[n++] = op;
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
    c.buf = (u32 *)start;
    c.pos = 0;
    c.cap = MAX_BLOCK_BYTES / 4;
    c.overflow = 0;

    emit_prologue(&c);
    emit_load_callee(&c, JR_TMP, fn_addr(helper_interp));
    for (i = 0; i < n; i++)
    {
        ppc_mr(&c, 3, JR_SYS);
        ppc_li32u(&c, 4, pc + (i + 1) * 4);
        emit_call_reg(&c, JR_TMP);
        ppc_cmpwi(&c, 0, 3, 0);
        exitFix[nFix++] = ppc_bc_fwd(&c, 4, 0, PPC_EQ);   // bne exit
        j->stats.helperInsns++;
    }
    for (i = 0; i < nFix; i++) ppc_patch_here(&c, exitFix[i]);
    emit_epilogue(&c);
    if (c.overflow)
    {
        H64_ERROR("[jit] block at %08X too large", pc);
        return 0;
    }

    b = &j->blocks[j->blockCount++];
    memset(b, 0, sizeof(*b));
    b->vpc = pc;
    b->paddr = paddr;
    b->insns = n;
    b->valid = 1;
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
    j->memUsed = (u32)((start - j->mem) + c.pos * 4);
    if (j->flushIcache) j->flushIcache(start, c.pos * 4);

    b->hashNext = j->hash[(pc >> 2) & 8191];
    j->hash[(pc >> 2) & 8191] = b;
    b->pageNext = j->pageHead[paddr >> 12];
    j->pageHead[paddr >> 12] = b;
    j->stats.blocksCompiled++;
    H64_DEBUG("[jit] block %08X (phys %06X): %u instructions, %u bytes, first %08X", pc, paddr, n, c.pos * 4, ops[0]);
    return b;
}
