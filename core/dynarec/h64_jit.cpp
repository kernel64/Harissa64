// Harissa64 V2 - recompiler: block cache, dispatcher, code generation.
#include "h64_jit.h"
#include "h64_jit_internal.h"

#include <stdlib.h>
#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"

// ---- Block cache ----
static u32 hash_of(u32 pc) { return (pc >> 2) & 8191; }

void h64_jit_reset(H64System *sys)
{
    H64Jit *j = sys->jit;
    if (!j) return;
    memset(j->hash, 0, sizeof(j->hash));
    memset(j->pageHead, 0, sizeof(j->pageHead));
    j->blockCount = 0;
    j->memUsed = 0;
    j->curInvalidated = 1;
    j->stats.flushes++;
}

int h64_jit_init(H64System *sys, void *execMem, u32 size, void (*flushIcache)(void *addr, u32 len))
{
    H64Jit *j;
    if (!H64_JIT_CAN_RUN || !execMem || size < 65536) return -1;
    j = (H64Jit *)calloc(1, sizeof(H64Jit));
    if (!j) return -1;
    j->mem = (u8 *)execMem;
    j->memSize = size;
    j->flushIcache = flushIcache;
    j->blockCap = 32768;
    j->blocks = (H64JitBlock *)calloc(j->blockCap, sizeof(H64JitBlock));
    if (!j->blocks) { free(j); return -1; }
    sys->jit = j;
    h64_jit_reset(sys);
    j->stats.flushes = 0;
    return 0;
}

void h64_jit_free(H64System *sys)
{
    if (!sys->jit) return;
    free(sys->jit->blocks);
    free(sys->jit);
    sys->jit = 0;
}

void h64_jit_invalidate(H64System *sys, u32 paddr, u32 len)
{
    H64Jit *j = sys->jit;
    u32 first, last, p;
    if (!j || !len || paddr >= H64_RDRAM_SIZE) return;
    first = paddr >> 12;
    last = (paddr + len - 1) >> 12;
    if (last >= H64_JIT_PAGES) last = H64_JIT_PAGES - 1;
    for (p = first; p <= last; p++)
    {
        H64JitBlock *b = j->pageHead[p];
        if (!b) continue;
        while (b) { b->valid = 0; b = b->pageNext; }
        j->pageHead[p] = 0;
        j->stats.invalidations++;
        if (p == j->curPage) j->curInvalidated = 1;
    }
}

int h64_jit_kernel_mode(const H64Cpu *cpu)
{
    u32 sr = (u32)cpu->cop0[CP0_STATUS];
    return (sr & (SR_EXL | SR_ERL)) || (sr & SR_KSU) == 0;
}

static H64JitBlock *lookup(H64Jit *j, u32 pc, u32 paddr, int kernel)
{
    H64JitBlock *b = j->hash[hash_of(pc)];
    for (; b; b = b->hashNext)
        if (b->valid && b->vpc == pc && b->paddr == paddr && b->kernel == (kernel && !j->noNative)) return b;
    return 0;
}

// ---- Dispatcher ----
static int interrupt_pending(const H64Cpu *cpu)
{
    u32 sr = (u32)cpu->cop0[CP0_STATUS];
    return (sr & SR_IE) && !(sr & (SR_EXL | SR_ERL)) && (cpu->cop0[CP0_CAUSE] & sr & SR_IM);
}

int h64_jit_should_exit(H64System *sys)
{
    H64Jit *j = sys->jit;
    return j->curInvalidated || sys->sched.next < j->blockEndCycles || interrupt_pending(&sys->cpu) || sys->stop;
}

static void process_events(H64System *sys)
{
    while (sys->sched.next <= sys->cpu.cycles)
    {
        int ev = h64_sched_pop_due(&sys->sched, sys->cpu.cycles);
        if (ev < 0) break;
        h64_device_event(sys, ev);
    }
}

void h64_jit_run_one(H64System *sys)
{
    H64Jit *j = sys->jit;
    H64Cpu *cpu = &sys->cpu;
    H64JitBlock *b;
    u32 pc32, paddr;

    process_events(sys);
    pc32 = (u32)cpu->pc;
    // Cases the recompiled code doesn't start in: delay slots, pending
    // interrupts, 64-bit addresses, misaligned pcs, code outside RDRAM.
    if (cpu->branchPending || interrupt_pending(cpu) || cpu->pc != (u64)(s64)(s32)pc32 || (pc32 & 3) ||
        h64_cpu_translate_debug(cpu, cpu->pc, &paddr) == 0 || paddr >= H64_RDRAM_SIZE)
    {
        h64_cpu_step(sys);
        j->stats.interpSteps++;
        return;
    }
    b = lookup(j, pc32, paddr, h64_jit_kernel_mode(cpu));
    if (!b)
    {
        {
            u64 t0 = h64_prof_now(sys);
            b = h64_jit_compile(sys, pc32, paddr);
            sys->prof[H64_PROF_JIT_COMPILE] += h64_prof_now(sys) - t0;
        }
        if (!b)
        {
            h64_cpu_step(sys);
            j->stats.interpSteps++;
            return;
        }
    }
    // Run the block only if no event falls inside it; otherwise one step.
    if (sys->sched.next < cpu->cycles + b->insns)
    {
        h64_cpu_step(sys);
        j->stats.interpSteps++;
        return;
    }
    j->blockEndCycles = cpu->cycles + b->insns;
    j->curPage = b->paddr >> 12;
    j->curInvalidated = 0;
    j->stats.blocksRun++;
    b->fn(sys);

    // Idle loop: skip whole iterations up to the next event. Nothing can
    // change the loop's outcome before an event (interrupts, DMA, the RSP
    // and the VI all arrive through the scheduler), so this equals running them.
    if (b->idle && b->valid && cpu->pc == (u64)(s64)(s32)b->vpc && !cpu->branchPending && !interrupt_pending(cpu) &&
        sys->sched.next != H64_NEVER && sys->sched.next > cpu->cycles)
    {
        u64 k = (sys->sched.next - cpu->cycles) / b->insns;
        if (b->idle == 2)
        {
            // The polled address must be plain RDRAM (a register read could have side effects).
            u64 a = cpu->gpr[b->pollBase] + (u64)(s64)b->pollOff;
            u32 a32 = (u32)a;
            if (a != (u64)(s64)(s32)a32 || (a32 >> 30) != 2 || (a32 & (b->pollSize - 1)) || (a32 & 0x1FFFFFFF) >= H64_RDRAM_SIZE)
                k = 0;
        }
        if (k > (1u << 24)) k = 1u << 24;
        if (k)
        {
            cpu->cycles += k * b->insns;
            cpu->instructions += k * b->insns;
            j->stats.idleSkipped += k * b->insns;
        }
    }
}

void h64_jit_run(H64System *sys, u64 cycles)
{
    u64 end = sys->cpu.cycles + cycles;
    sys->stop = 0;
    while (sys->cpu.cycles < end && !sys->stop)
        h64_jit_run_one(sys);
}
