// Harissa64 V2 - RSP (Reality Signal Processor), low-level emulation.
//
// Reference interpreter: scalar unit (a 32-bit MIPS subset), vector unit
// (32 registers of 8 x 16-bit elements, 48-bit accumulators, flags,
// reciprocal unit), SP registers and SP DMA. Ported from ares (ISC licence,
// ares/n64/rsp, commit a776c509, see THIRD_PARTY.md), scalar paths only.
//
// Timing: ares's pipeline model (ares/n64/rsp/rsp.cpp, rsp.hpp, decoder.cpp
// at commit 652d6537): one issue group per RCP cycle (62.5 MHz, 2 RCP cycles
// every 3 CPU cycles), a vector and a scalar instruction may issue together,
// and a group stalls when it reads a register a recent load wrote, stores
// right after a load, or sits in a taken branch's delay slot. A running RSP advances in slices driven by the scheduler (H64_EV_RSP every
// H64_RSP_SLICE CPU cycles) and catches up to the exact cycle whenever the
// CPU touches the RCP (h64_rsp_sync from the bus). Its effects on the CPU
// therefore don't depend on how CPU instructions are grouped (interpreter
// step by step, or dynarec blocks), which keeps the two in lockstep.
#ifndef H64_RSP_H
#define H64_RSP_H

#define H64_RSP_SLICE 512u   // CPU cycles

#include "../common/h64_types.h"

struct H64System;

struct H64SpDma
{
    u32 memAddr;     // bit 12: IMEM; bits 3..11: address
    u32 dramAddr;    // bits 3..23
    u32 length;      // bits 3..11 (length - 1, rounded to 8 bytes)
    u32 count;
    u32 skip;
    int toRdram;
};

// What an instruction reads and writes, for the pipeline model (ares OpInfo).
struct H64RspOp
{
    u32 rUse, rDef;    // scalar registers (bit n = register n)
    u32 vUse, vDef;    // vector registers
    u32 vfake;         // "fake" vector uses (dual-issue conflicts only)
    u16 flags;         // H64_RSPOP_*
    u8 vcUse, vcDef;   // VCO, VCC, VCE
};

struct H64RspStage
{
    u32 rWrite, vWrite;
    u32 load;
};

struct H64Rsp
{
    // Scalar unit
    u32 r[32];
    u32 pc, nextPc;            // 12-bit IMEM addresses
    // Vector unit (element 0 is the first in memory order)
    u16 vr[32][8];
    u16 acch[8], accm[8], accl[8];
    u16 vcoh[8], vcol[8], vcch[8], vccl[8], vce[8];   // 0 or 0xFFFF per element
    s16 divin, divout;
    int divdp;
    u16 reciprocals[512];
    u16 inverseSquareRoots[512];

    // SP registers
    u32 status;                // SP_STATUS bits (halt, broke, sstep, intr break, signals)
    u32 semaphore;
    H64SpDma pending, current;
    int dmaFull, dmaBusy;

    s32 cycleFrac;             // CPU cycles not yet turned into RCP cycles (x2; negative: cycles owed)
    // Pipeline (ares): the last three issue groups, and what the next one may do
    H64RspStage stage[3];
    int singleIssue;           // the next group issues one instruction
    int delaySlot;             // the next instruction is the delay slot of a taken branch
    int branchTaken;           // set by the instruction being run (not state)
    u32 delayTarget;
    u64 stalls, dualIssues;    // statistics
    u64 syncedCycles;          // CPU cycle the RSP has run up to
    int inSync;                // the RSP is running (its own register accesses must not re-enter)
    u64 instructions;
    u32 tasks;                 // tasks started (logged)
    int hleBusy;               // an HLE task is "running": the RSP halts at the next H64_EV_RSP
    int hleExtended;     // gfx timing: the task runs on to its estimated end (a yield request ends it)
    u32 hleStatus;             // SP_STATUS bits the HLE task sets when it ends
    int hleDpInterrupt;        // the HLE task ended with an RDP full sync: raise the DP interrupt too
    u32 hleTasks;              // tasks run by the HLE (statistics)
    u64 hleStart;              // CPU cycle the HLE task started
    u64 clocks;                // RCP cycles run by the LLE RSP
    int gfxMeasure;            // --gfx-cost-log: this LLE graphics task's HLE counts below
    u64 gfxClocks0;
    u32 gfxVerts, gfxTris, gfxCommands;
};

// Decoded instructions, one per IMEM word (checked against the word; not
// state). Kept at the end of H64System, away from the fields the dynarec
// reaches with 16-bit displacements.
struct H64RspDecode
{
    u32 word[1024];
    H64RspOp info[1024];
};

void h64_rsp_reset(H64System *sys);
// Runs the RSP for the RCP cycles matching `cpuCycles` CPU cycles (no-op while halted).
void h64_rsp_advance(H64System *sys, u32 cpuCycles);
// Runs the RSP up to the current CPU cycle.
void h64_rsp_sync(H64System *sys);
void h64_rsp_slice_event(H64System *sys);   // scheduler: H64_EV_RSP
// One instruction (for tests).
void h64_rsp_step(H64System *sys);
// One issue group (one or two instructions); returns its length in RCP cycles.
u32 h64_rsp_issue(H64System *sys);

// SP registers at 0x04040000 (reg 0..7) and SP_PC at 0x04080000.
u32 h64_sp_read(H64System *sys, u32 reg);
void h64_sp_write(H64System *sys, u32 reg, u32 value);
u32 h64_sp_pc_read(H64System *sys);
void h64_sp_pc_write(H64System *sys, u32 value);
void h64_sp_dma_event(H64System *sys);   // scheduler: the current DMA block is done

// Vector unit (h64_rsp_vu.cpp).
void h64_rsp_vu_init_tables(H64Rsp *rsp);
void h64_rsp_cop2(H64System *sys, u32 op);
void h64_rsp_lwc2(H64System *sys, u32 op);
void h64_rsp_swc2(H64System *sys, u32 op);

#endif
