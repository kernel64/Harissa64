// Harissa64 V2 - recompiler internals shared by h64_jit.cpp and h64_jit_gen.cpp.
#ifndef H64_JIT_INTERNAL_H
#define H64_JIT_INTERNAL_H

#include "h64_jit.h"

struct H64System;
struct H64Cpu;

typedef void (*H64JitFn)(H64System *sys);

struct H64JitBlock
{
    u32 vpc, paddr;
    u32 insns;           // MIPS instructions
    int valid;
    int kernel;          // compiled in kernel mode (native code allowed)
    // Idle loop (a branch back to the block start with no side effects):
    // 1 = pure loop, 2 = RDRAM poll (load from pollBase + pollOff, size pollSize).
    int idle;
    u32 pollBase, pollSize;
    s32 pollOff;
    H64JitFn fn;         // callable (an ELFv1 function descriptor on ppc64 Linux)
    u32 *body;           // first instruction after the prologue (linked exits jump here)
    int linkHead;        // first link into this block (-1: none)
    H64JitBlock *hashNext, *pageNext;
};

H64JitBlock *h64_jit_compile(H64System *sys, u32 pc, u32 paddr);
// Writes the shared entry/exit/check code at the start of the code memory (after a reset).
void h64_jit_emit_runtime(H64System *sys);
void h64_jit_code_map(H64Jit *j, const H64JitBlock *b, int delta);
// Kernel mode (Status.KSU = 0, or EXL/ERL set): native code is allowed.
int h64_jit_kernel_mode(const H64Cpu *cpu);
// After a helper: leave the block? (invalidated page, event due inside the block, interrupt, stop)
int h64_jit_should_exit(H64System *sys);

// Generated code registers (non-volatile, saved by the block prologue).
enum { JR_SYS = 31, JR_CPU = 30, JR_RDRAM = 29, JR_TMP = 28 };

#endif
