// Harissa64 V2 - MIPS -> PowerPC dynamic recompiler.
//
// Blocks of up to H64_JIT_MAX_INSNS MIPS instructions, inside one 4 KB page,
// ending after a branch and its delay slot. Instructions are emitted as
// native PowerPC code when the recompiler knows them, otherwise as a call to
// the reference interpreter for that one instruction. All MIPS state stays
// in H64Cpu (no register caching yet), so the interpreter and the
// recompiled code can hand over at any instruction.
//
// Timing is exactly the interpreter's: the dispatcher runs a block only when
// no scheduler event falls inside it (otherwise it steps the interpreter),
// and a block leaves as soon as something changes that (an exception, a pc
// other than the expected one, a new event before the block's end, a pending
// interrupt, or a store into the block's own page). h64test --lockstep runs
// the interpreter next to it and compares the whole CPU state after every
// block.
//
// Execution needs a PowerPC host: Xbox 360 (XDK ABI: plain function
// addresses, 32-bit pointers) or ppc64 big-endian Linux (ELFv1: function
// descriptors, TOC in r2), used under QEMU for tests. Elsewhere the
// recompiler compiles (and its encoder is unit-tested) but cannot run.
#ifndef H64_JIT_H
#define H64_JIT_H

#include "../common/h64_types.h"

#if defined(_XBOX)
#define H64_JIT_ABI_XBOX 1
#define H64_JIT_CAN_RUN 1
#elif defined(__powerpc64__) && defined(__BIG_ENDIAN__) && (!defined(_CALL_ELF) || _CALL_ELF == 1)
#define H64_JIT_ABI_ELFV1 1
#define H64_JIT_CAN_RUN 1
#else
#define H64_JIT_CAN_RUN 0
#endif

#define H64_JIT_MAX_INSNS 64
#define H64_JIT_PAGES (0x800000u >> 12)   // RDRAM pages (8 MB)

struct H64System;
struct H64JitBlock;

struct H64JitStats
{
    u64 blocksRun, blocksCompiled, interpSteps, invalidations, flushes, earlyExits, idleSkipped;
    u64 nativeInsns, helperInsns;   // compiled instructions, by kind
};

struct H64Jit
{
    u8 *mem;                        // executable code memory (from the platform)
    u32 memSize, memUsed;
    void (*flushIcache)(void *addr, u32 len);

    H64JitBlock *blocks;            // block pool
    u32 blockCount, blockCap;
    H64JitBlock *hash[8192];        // by (u32)pc
    H64JitBlock *pageHead[H64_JIT_PAGES];   // live blocks of each RDRAM page (NULL: no code)

    // The block being run (for the early-exit checks).
    u64 blockEndCycles;
    u32 curPage;
    int curInvalidated;

    H64JitStats stats;
    u32 *opHist;          // optional (debug, 232 entries): instructions run through the interpreter helper,
                          // by opcode: [op], [64 + SPECIAL funct], [128 + COP1 S/D funct], [200 + COP1 rs]
                          // for the moves and branches, [216 + 0/1] CVT.S/CVT.D from W/L
    int noNative;                   // debugging: every instruction through the interpreter
    int noFpu;                      // debugging: COP1 arithmetic through the interpreter
};

// Takes executable memory (and its icache flush) from the platform.
int h64_jit_init(H64System *sys, void *execMem, u32 size, void (*flushIcache)(void *addr, u32 len));
void h64_jit_free(H64System *sys);
void h64_jit_reset(H64System *sys);   // drops every block
// Runs until `cycles` more CPU cycles have elapsed or sys->stop is set.
void h64_jit_run(H64System *sys, u64 cycles);
// One dispatch: one block, or one interpreter step (used by the lockstep check).
void h64_jit_run_one(H64System *sys);
// RDRAM [paddr, paddr + len) was written: drops the blocks of those pages.
void h64_jit_invalidate(H64System *sys, u32 paddr, u32 len);

#endif
