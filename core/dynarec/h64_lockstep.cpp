// Harissa64 V2 - lockstep check: the recompiler against the reference interpreter.
#include "h64_lockstep.h"

#include <stdio.h>
#include <string.h>

#include "../system/h64_system.h"

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

static int compare_cpu(const H64System *a, const H64System *b, char *why, size_t whyLen)
{
    const H64Cpu *x = &a->cpu, *y = &b->cpu;
    int i;
    for (i = 0; i < 32; i++)
        if (x->gpr[i] != y->gpr[i])
        {
            snprintf(why, whyLen, "gpr[%d] interp %016llX jit %016llX", i, (unsigned long long)x->gpr[i], (unsigned long long)y->gpr[i]);
            return 1;
        }
    for (i = 0; i < 32; i++)
        if (x->fgr[i] != y->fgr[i])
        {
            snprintf(why, whyLen, "fgr[%d] interp %016llX jit %016llX", i, (unsigned long long)x->fgr[i], (unsigned long long)y->fgr[i]);
            return 1;
        }
    for (i = 0; i < 32; i++)
        if (i != CP0_RANDOM && x->cop0[i] != y->cop0[i])
        {
            snprintf(why, whyLen, "cop0[%d] interp %016llX jit %016llX", i, (unsigned long long)x->cop0[i], (unsigned long long)y->cop0[i]);
            return 1;
        }
    if (x->hi != y->hi || x->lo != y->lo) { snprintf(why, whyLen, "hi/lo"); return 1; }
    if (x->pc != y->pc || x->nextPc != y->nextPc || x->branchPending != y->branchPending)
    {
        snprintf(why, whyLen, "pc interp %016llX/%016llX/%d jit %016llX/%016llX/%d", (unsigned long long)x->pc,
                 (unsigned long long)x->nextPc, x->branchPending, (unsigned long long)y->pc, (unsigned long long)y->nextPc,
                 y->branchPending);
        return 1;
    }
    if (x->fcr31 != y->fcr31 || x->llbit != y->llbit) { snprintf(why, whyLen, "fcr31/llbit"); return 1; }
    if (x->instructions != y->instructions)
    {
        snprintf(why, whyLen, "instruction count interp %llu jit %llu", (unsigned long long)x->instructions,
                 (unsigned long long)y->instructions);
        return 1;
    }
    return 0;
}

int h64_lockstep_run(H64System *ref, H64System *jit, u64 limitCycles, H64LockstepResult *res)
{
    u64 n = 0;
    memset(res, 0, sizeof(*res));
    while (jit->cpu.cycles < limitCycles && !ref->exitRequested && !jit->exitRequested)
    {
        u64 before = jit->cpu.cycles;
        u32 pc = (u32)jit->cpu.pc;
        int bad;
        h64_jit_run_one(jit);
        while (ref->cpu.cycles < jit->cpu.cycles) h64_system_step(ref);
        n++;
        if (ref->cpu.cycles != jit->cpu.cycles)
        {
            snprintf(res->why, sizeof(res->why), "cycles interp %llu jit %llu", (unsigned long long)ref->cpu.cycles,
                     (unsigned long long)jit->cpu.cycles);
            bad = 1;
        }
        else
            bad = compare_cpu(ref, jit, res->why, sizeof(res->why));
        if (!bad && (n & 0x3FFF) == 0 && memcmp(ref->rdram, jit->rdram, H64_RDRAM_SIZE))
        {
            snprintf(res->why, sizeof(res->why), "RDRAM differs");
            bad = 1;
        }
        if (bad)
        {
            res->diverged = 1;
            res->pc = pc;
            res->cyclesBefore = before;
            res->frame = jit->vi.frames;
            break;
        }
    }
    if (!res->diverged && memcmp(ref->rdram, jit->rdram, H64_RDRAM_SIZE))
    {
        res->diverged = 1;
        snprintf(res->why, sizeof(res->why), "RDRAM differs at the end");
    }
    res->dispatches = n;
    return res->diverged;
}
