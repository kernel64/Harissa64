// Harissa64 V2 - recompiler internals shared by h64_jit.cpp and h64_jit_gen.cpp.
#ifndef H64_JIT_INTERNAL_H
#define H64_JIT_INTERNAL_H

#include "h64_jit.h"

struct H64System;

typedef void (*H64JitFn)(H64System *sys);

struct H64JitBlock
{
    u32 vpc, paddr;
    u32 insns;           // MIPS instructions
    int valid;
    H64JitFn fn;         // callable (an ELFv1 function descriptor on ppc64 Linux)
    H64JitBlock *hashNext, *pageNext;
};

H64JitBlock *h64_jit_compile(H64System *sys, u32 pc, u32 paddr);
// After a helper: leave the block? (invalidated page, event due inside the block, interrupt, stop)
int h64_jit_should_exit(H64System *sys);

// Generated code registers (non-volatile, saved by the block prologue).
enum { JR_SYS = 31, JR_CPU = 30, JR_RDRAM = 29, JR_TMP = 28 };

#endif
