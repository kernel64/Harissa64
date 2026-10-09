// Harissa64 V2 - NEC VR4300 (R4300i) CPU state and reference interpreter.
//
// Written from the VR4300 user manual and n64brew. The interpreter is the
// reference implementation: simple, one instruction at a time, every check
// done (address errors, overflow traps, TLB, interrupts before each
// instruction). The dynarec (M3) is compared against it in lockstep.
#ifndef H64_CPU_H
#define H64_CPU_H

#include "../common/h64_types.h"

struct H64System;

// COP0 register numbers.
enum
{
    CP0_INDEX = 0, CP0_RANDOM = 1, CP0_ENTRYLO0 = 2, CP0_ENTRYLO1 = 3, CP0_CONTEXT = 4, CP0_PAGEMASK = 5,
    CP0_WIRED = 6, CP0_BADVADDR = 8, CP0_COUNT = 9, CP0_ENTRYHI = 10, CP0_COMPARE = 11, CP0_STATUS = 12,
    CP0_CAUSE = 13, CP0_EPC = 14, CP0_PRID = 15, CP0_CONFIG = 16, CP0_LLADDR = 17, CP0_WATCHLO = 18,
    CP0_WATCHHI = 19, CP0_XCONTEXT = 20, CP0_PARITYERROR = 26, CP0_CACHEERROR = 27, CP0_TAGLO = 28,
    CP0_TAGHI = 29, CP0_ERROREPC = 30
};

// Status bits.
#define SR_IE  0x00000001u
#define SR_EXL 0x00000002u
#define SR_ERL 0x00000004u
#define SR_KSU 0x00000018u
#define SR_UX  0x00000020u
#define SR_SX  0x00000040u
#define SR_KX  0x00000080u
#define SR_IM  0x0000FF00u
#define SR_BEV 0x00400000u
#define SR_FR  0x04000000u
#define SR_CU0 0x10000000u
#define SR_CU1 0x20000000u

// Exception codes (Cause.ExcCode).
enum
{
    EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7,
    EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15, EXC_WATCH = 23
};

// FCR31 bits.
#define FCR31_RM_MASK  0x00000003u
#define FCR31_FLAGS    0x0000007Cu   // I U O Z V  (bits 2..6)
#define FCR31_ENABLES  0x00000F80u   // I U O Z V  (bits 7..11)
#define FCR31_CAUSE    0x0003F000u   // I U O Z V E (bits 12..17)
#define FCR31_C        0x00800000u
#define FCR31_FS       0x01000000u
#define FCR31_WRITABLE 0x0183FFFFu

struct H64TlbEntry
{
    u32 pageMask;   // PageMask as written (bits 13..24)
    u64 entryHi;    // VPN2 | ASID (R bits and VPN2, as written)
    u32 entryLo0;   // PFN | C | D | V | G
    u32 entryLo1;
};

struct H64Cpu
{
    u64 gpr[32];
    u64 hi, lo;

    u32 lastOp;        // the last instruction fetched (statistics)
    u64 pc;            // next instruction to execute
    u64 nextPc;        // the one after it (differs after a taken branch)
    int branchPending; // the instruction at pc is in a branch delay slot

    // State of the instruction being executed (for exceptions).
    u64 curPc;
    int curInDelaySlot;

    u64 cop0[32];
    u32 countOffset;   // Count = (cycles >> 1) + countOffset
    u64 cop0Latch;     // value read back from the unimplemented COP0 registers
    u64 cop2Latch;     // COP2 does not exist; its moves go through one 64-bit latch
    int llbit;

    u64 fgr[32];
    u32 fcr31;
    double jitFpMin[2];   // dynarec native FPU: smallest normal single and double
    u64 jitScratch;       // dynarec native FPU: FPSCR read-back

    H64TlbEntry tlb[32];

    u64 cycles;        // PClock cycles since power-on
    u32 cpi;           // PClock cycles per instruction (1 = the reference model; mupen64plus's
                       // CountPerOp N is 2N: fewer instructions per frame in busy-wait loops)
    u64 instructions;  // executed instructions (statistics)
    int exceptionRaised;

    // Last executed PCs (debugging: h64test --trace-exc prints them).
    u32 pcHistory[32];
    u32 pcHistoryPos;
    u32 jumpFrom[16], jumpTo[16];   // last taken jumps/branches
    u32 jumpPos;

    // Debug hook called for every exception taken (h64test --trace-exc).
    void (*excHook)(void *user, int code);
    u32 watchPc;                                   // debugging: call watchHook when executing this PC
    u32 jumpLimit;                                 // debugging: call jumpHook for a jump to a KSEG0 address >= this
    void (*watchHook)(void *user);
    void (*jumpHook)(void *user, u32 from, u32 to);
    u32 nopRun;                                    // consecutive NOPs (debugging: nopHook at 1000)
    void (*nopHook)(void *user);
    void *excUser;
    u32 tlbGen;        // changes with every TLB write and EntryHi (ASID) write: cached translations expire
};

void h64_cpu_reset(H64Cpu *cpu);
// Executes one instruction (or takes a pending interrupt).
void h64_cpu_step(H64System *sys);

// COP0 helpers shared with the system.
u32 h64_cpu_count(const H64Cpu *cpu);
void h64_cpu_set_ip(H64System *sys, int bit, int on);   // Cause.IP bits 2..7 (bit 2 = RCP, 7 = timer)
void h64_cpu_reschedule_compare(H64System *sys);

// Virtual -> physical for debugging tools; returns 0 when unmapped.
int h64_cpu_translate_debug(H64Cpu *cpu, u64 vaddr, u32 *paddr);
// Instruction fetch at a 32-bit address in the current mode, without side
// effects (no exception, no copy): 1 with *paddr, 0 when it would fault.
int h64_cpu_probe_fetch(const H64Cpu *cpu, u32 a, u32 *paddr);
// One step with the instruction at pc already known (the recompiler, for an
// instruction it read when compiling the block): the same as h64_cpu_step
// without the fetch.
struct H64System;
void h64_cpu_step_op(struct H64System *sys, u32 op);
// The instruction at cpu->pc when it is in RDRAM (statistics, no side
// effects). Returns 0 on success.
struct H64System;
int h64_cpu_peek_op(struct H64System *sys, u32 *op);

#endif
