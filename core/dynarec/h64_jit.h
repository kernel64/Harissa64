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
    u64 helperCalls;                // instructions run through the interpreter helper (incl. slow paths)
    u64 codeBytes, hotBytes;        // generated code: all of it, and the part before the cold slow paths
};

// A patched block exit: a direct branch into another block's body.
struct H64JitLink
{
    u32 *patch;                     // the branch word in the source block
    u32 orig;                       // its unlinked value
    int next;                       // next link into the same target (-1: none); free: next free record
    int tlb;                        // the target is TLB-mapped: undone when its TLB entry changes
    int target;                     // index of the target block
    int tlbEntry;                   // TLB-mapped target: the TLB entry it went through (-1: none)
    int tlbPrev, tlbNext;           // the other links through that entry (H64Jit.tlbHead)
};

struct H64Jit
{
    // Read and written by generated code: kept first, at small offsets.
    u64 blockEndCycles;             // the block being run: leave it if an event falls before this
    u64 runEnd;                     // h64_jit_run's end: chained blocks stop there
    u32 *lastExit;                  // the unlinked exit the last block left by (NULL: none)
    u32 lastExitTarget;             // and the MIPS pc it goes to
    u32 curPage;
    int curInvalidated;
    u16 *codeMap;                   // live blocks over each 64-byte RDRAM chunk (stores there take the slow path)
    // Indirect jump targets (jr/jalr, read by rtIndirect): KSEG0/1 pc -> native
    // block body, by pc bits 2..11; filled by the dispatcher, dropped with their block.
    u32 *indPc;
    u32 **indBody;

    u8 *mem;                        // executable code memory (from the platform)
    u32 memSize, memUsed;           // hot code grows from the start...
    u32 coldBase, coldUsed;         // ...and the slow paths from coldBase (half of the memory)
    void (*flushIcache)(void *addr, u32 len);

    H64JitBlock *blocks;            // block pool
    u32 blockCount, blockCap;
    H64JitBlock *hash[8192];        // by (u32)pc
    H64JitBlock *pageHead[H64_JIT_PAGES];   // live blocks of each RDRAM page (NULL: no code)

    // Shared code at the start of the code memory (h64_jit_emit_runtime):
    // the entry (prologue, then the block's body), the exit (epilogue) and the
    // address checks of native loads and stores, so blocks stay small.
    void (*enter)(H64System *sys, u32 *body);   // an ELFv1 descriptor on ppc64 Linux
    u32 *rtExit;
    u32 *rtCheck[2][4];             // [store][log2 size]: r3 = address -> r4 = physical, cr0.eq = fast path
    u32 *rtFpCheck[2][2];           // [double][two operands]: f1 (and f2) normal, zero or infinite -> cr0.eq
    u32 *rtFpFinish[3];             // [result: none, single, double]: FPSCR and FCR31 after an operation -> cr0.eq
    u32 *rtSlow;                    // a slow path's interpreter call (see h64_jit_emit_runtime)
    u32 *rtIndirect;                // a jr/jalr exit: straight into the target's block when known
    u32 *rtLink;                    // a fixed-target exit: counters, then go on (cr0.eq) or leave
    u32 *rtFcStore, *rtFcLoad;      // slow paths: cached FGRs to/from H64Cpu, from a table after the call


    // Block linking: exits with a fixed target jump straight into the next
    // block's body once it has been compiled (see h64_jit_gen.cpp).
    H64JitLink *links;
    u32 linkCount, linkCap;
    int noLink;                     // debugging: every block returns to the dispatcher
    int fullExits;                  // debugging: linked exits without rtLink (the inline form)

    H64JitStats stats;
    u32 *opHist;          // optional (debug, 232 entries): instructions run through the interpreter helper,
                          // by opcode: [op], [64 + SPECIAL funct], [128 + COP1 S/D funct], [200 + COP1 rs]
                          // for the moves and branches, [216 + 0/1] CVT.S/CVT.D from W/L
    int noNative;                   // debugging: every instruction through the interpreter
    int noFpu;                      // debugging: COP1 arithmetic through the interpreter
    int noRegCache;                 // debugging: no MIPS registers kept in host registers
    int noFpCache;                  // debugging: no COP1 registers kept in host FPRs
    int noFpuGuard;                 // debugging: COP1 state checked at every instruction
    void *dumpFile;                 // debugging (h64test --jit-dump): a FILE * receiving each compiled block
    int fastFpu;                    // native COP1 without reading FPSCR: FCR31's Inexact cause/flag not kept
                                    // (values unchanged); set before h64_jit_reset, which emits the runtime

    // Translations of TLB-mapped instruction pages for the dispatcher
    // (Perfect Dark, GoldenEye and Conker run code at 0x70000000/0x7F000000):
    // valid while cpu.tlbGen is unchanged; key = page | mode bits.
    // valid while that TLB entry and the ASID are unchanged (entry -1: KSEG0/1).
    struct { u32 key, ppage, gen, asid; int entry; } fetch[256];
    u32 linkTlbGen;                 // cpu.tlbGen last seen by the dispatcher
    u32 seenEntryGen[32], seenAsidGen;
    // Links into TLB-mapped code, one list per TLB entry (-1: empty).
    int tlbHead[32];
    u32 tlbLinkCount;
    int freeLink;                   // first free link record (-1: none)
    u32 rtCpi;                      // cpu.cpi the runtime and the blocks were generated for
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
