// Harissa64 V2 - lockstep check: runs a recompiler system one dispatch at a
// time and a reference (interpreter) system up to the same cycle, and
// compares the whole CPU state after every dispatch and RDRAM regularly.
#ifndef H64_LOCKSTEP_H
#define H64_LOCKSTEP_H

#include "../common/h64_types.h"

struct H64System;

struct H64LockstepResult
{
    int diverged;
    u64 dispatches;
    u32 pc;              // block start of the diverging dispatch
    u64 cyclesBefore;
    u32 frame;
    char why[256];
};

// Both systems must start from the same state (same ROM, same options).
// Returns 1 on a divergence.
int h64_lockstep_run(H64System *ref, H64System *jit, u64 limitCycles, H64LockstepResult *res);

#endif
