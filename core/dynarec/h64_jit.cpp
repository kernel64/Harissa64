// Harissa64 V2 - recompiler: block cache, dispatcher, code generation.
#include "h64_jit.h"
#include "h64_jit_internal.h"

#include <stddef.h>
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
    memset(j->fetch, 0xFF, sizeof(j->fetch));
    j->tlbLinkCount = 0;
    memset(j->tlbHead, 0xFF, sizeof(j->tlbHead));
    memset(j->indPc, 0, 1024 * sizeof(u32));
    memset(j->indBody, 0, 1024 * sizeof(u32 *));
    memset(j->codeMap, 0, (H64_RDRAM_SIZE >> 6) * sizeof(u16));
    j->blockCount = 0;
    j->linkCount = 0;
    j->freeLink = -1;
    j->lastExit = 0;
    j->memUsed = 0;
    // Slow paths take about 1.4 times the hot code (DK64: 48 hot and 69 cold
    // bytes per MIPS instruction; with half each the cold half filled first).
    j->coldBase = (j->memSize / 12 * 5) & ~63u;
    j->coldUsed = j->coldBase;
    h64_jit_emit_runtime(sys);   // sets memUsed past the shared code
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
    j->linkCap = 65536;
    j->links = (H64JitLink *)calloc(j->linkCap, sizeof(H64JitLink));
    j->codeMap = (u16 *)calloc(H64_RDRAM_SIZE >> 6, sizeof(u16));
    j->indPc = (u32 *)calloc(1024, sizeof(u32));
    j->indBody = (u32 **)calloc(1024, sizeof(u32 *));
    if (!j->blocks || !j->links || !j->codeMap || !j->indPc || !j->indBody)
    {
        free(j->blocks); free(j->links); free(j->codeMap); free(j->indPc); free(j->indBody); free(j);
        return -1;
    }
    j->runEnd = ~0ull;
    // Linked exits read sys->sched.next and sys->jit with 16-bit displacements.
    if (offsetof(H64System, sched) + offsetof(H64Scheduler, next) > 32760 || offsetof(H64System, jit) > 32760 ||
        offsetof(H64System, mi) > 32760)
    {
        H64_WARN("[jit] H64System fields out of reach of linked exits: block linking off");
        j->noLink = 1;
    }
    sys->jit = j;
    h64_jit_reset(sys);
    j->stats.flushes = 0;
    return 0;
}

void h64_jit_free(H64System *sys)
{
    if (!sys->jit) return;
    free(sys->jit->blocks);
    free(sys->jit->links);
    free(sys->jit->codeMap);
    free(sys->jit->indPc);
    free(sys->jit->indBody);
    free(sys->jit);
    sys->jit = 0;
}

// Counts b in (+1) or out (-1) of the 64-byte chunks it covers.
void h64_jit_code_map(H64Jit *j, const H64JitBlock *b, int delta)
{
    u32 a = b->paddr >> 6, e = (b->paddr + b->insns * 4 - 1) >> 6;
    for (; a <= e && a < (H64_RDRAM_SIZE >> 6); a++) j->codeMap[a] = (u16)(j->codeMap[a] + delta);
}

// Link records are reused: Conker relinks tens of thousands of exits a frame
// (its TLB refills keep remapping code pages), and records never given back
// filled the table within seconds, after which nothing was linked any more.
static void tlb_list_add(H64Jit *j, int i, int e)
{
    H64JitLink *l = &j->links[i];
    l->tlbEntry = e;
    l->tlbPrev = -1;
    l->tlbNext = j->tlbHead[e];
    if (l->tlbNext >= 0) j->links[l->tlbNext].tlbPrev = i;
    j->tlbHead[e] = i;
    j->tlbLinkCount++;
}

static void tlb_list_remove(H64Jit *j, int i)
{
    H64JitLink *l = &j->links[i];
    if (l->tlbEntry < 0) return;
    if (l->tlbPrev >= 0) j->links[l->tlbPrev].tlbNext = l->tlbNext;
    else j->tlbHead[l->tlbEntry] = l->tlbNext;
    if (l->tlbNext >= 0) j->links[l->tlbNext].tlbPrev = l->tlbPrev;
    l->tlbEntry = -1;
    j->tlbLinkCount--;
}

static void free_link(H64Jit *j, int i)
{
    H64JitLink *l = &j->links[i];
    tlb_list_remove(j, i);
    l->patch = 0;
    l->next = j->freeLink;
    j->freeLink = i;
}

// Puts back the exits linked into b (it is being invalidated).
static void unlink_block(H64Jit *j, H64JitBlock *b)
{
    int i = b->linkHead;
    while (i >= 0)
    {
        H64JitLink *l = &j->links[i];
        int next = l->next;
        *l->patch = l->orig;
        if (j->flushIcache) j->flushIcache(l->patch, 4);
        free_link(j, i);
        i = next;
    }
    b->linkHead = -1;
    {
        u32 k = (b->vpc >> 2) & 1023;   // its indirect-jump entry
        if (j->indBody[k] == b->body) { j->indBody[k] = 0; j->indPc[k] = 0; }
    }
}

void h64_jit_invalidate(H64System *sys, u32 paddr, u32 len)
{
    H64Jit *j = sys->jit;
    u32 first, last, p;
    if (!j || !len || paddr >= H64_RDRAM_SIZE) return;
    first = paddr >> 12;
    last = (paddr + len - 1) >> 12;
    if (last >= H64_JIT_PAGES) last = H64_JIT_PAGES - 1;
    // Only the blocks the write overlaps: data often shares a page with code
    // (OoT invalidated ~500 pages a second when whole pages were dropped).
    for (p = first; p <= last; p++)
    {
        H64JitBlock **link = &j->pageHead[p], *b;
        int hit = 0;
        while ((b = *link) != 0)
        {
            if (b->paddr < paddr + len && paddr < b->paddr + b->insns * 4)
            {
                b->valid = 0;
                unlink_block(j, b);
                h64_jit_code_map(j, b, -1);
                *link = b->pageNext;
                hit = 1;
            }
            else
                link = &b->pageNext;
        }
        if (!hit) continue;
        j->stats.invalidations++;
        // Linked blocks run without the dispatcher updating curPage: any
        // invalidation stops the block being run.
        j->curInvalidated = 1;
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
    return j->curInvalidated || sys->sched.next < j->blockEndCycles || interrupt_pending(&sys->cpu) || sys->stop ||
           sys->cpu.tlbGen != j->linkTlbGen;   // linked jumps into TLB-mapped code may be stale
}

static void process_events(H64System *sys)
{
    u64 t0, nested;
    if (sys->sched.next > sys->cpu.cycles) return;
    t0 = h64_prof_now(sys);
    nested = h64_prof_nested(sys);
    while (sys->sched.next <= sys->cpu.cycles)
    {
        int ev = h64_sched_pop_due(&sys->sched, sys->cpu.cycles);
        if (ev < 0) break;
        h64_device_event(sys, ev);
    }
    h64_prof_add_outer(sys, H64_PROF_EVENTS, t0, nested);
}

// Checks the links into TLB-mapped code that went through TLB entry `entry`
// (-2: all of them, the ASID changed): a link stays while its target's virtual
// pc still maps to the block's physical address (an entry maps two pages and
// Conker often rewrites only one of them), otherwise it is undone.
static void unlink_tlb_entry(H64System *sys, int entry)
{
    H64Jit *j = sys->jit;
    int i = j->tlbHead[entry];
    while (i >= 0)
    {
        H64JitLink *l = &j->links[i];
        H64JitBlock *b = &j->blocks[l->target];
        int next = l->tlbNext, e;
        u32 paddr;
        if (b->valid && h64_cpu_probe_fetch(&sys->cpu, b->vpc, &paddr, &e) && e >= 0 && paddr == b->paddr)
        {
            if (e != entry)
            {
                tlb_list_remove(j, i);
                tlb_list_add(j, i, e);
            }
        }
        else
        {
            int *p = &b->linkHead;   // out of the target's list
            *l->patch = l->orig;
            if (j->flushIcache) j->flushIcache(l->patch, 4);
            while (*p >= 0 && *p != i) p = &j->links[*p].next;
            if (*p == i) *p = l->next;
            free_link(j, i);
        }
        i = next;
    }
}

static void unlink_tlb(H64System *sys, int entry)
{
    int e;
    if (entry >= 0) { unlink_tlb_entry(sys, entry); return; }
    for (e = 0; e < 32; e++) unlink_tlb_entry(sys, e);
}

// Links the exit the previous block left by to b (the block it leads to).
static void link_exit(H64Jit *j, u32 *patch, H64JitBlock *b, int tlb)
{
    H64JitLink *l;
    s32 off;
    int i;
    if (!b->body) return;
    off = (s32)((u8 *)b->body - (u8 *)patch);
    if (off < -0x2000000 || off >= 0x2000000) return;
    if (*patch == (0x48000000u | ((u32)off & 0x03FFFFFCu))) return;   // linked already: no second record
    if (j->freeLink >= 0)
    {
        i = j->freeLink;
        j->freeLink = j->links[i].next;
    }
    else if (j->linkCount < j->linkCap)
        i = (int)j->linkCount++;
    else
        return;
    l = &j->links[i];
    l->patch = patch;
    l->orig = *patch;
    l->tlb = tlb >= 0;
    l->target = (int)(b - j->blocks);
    l->tlbEntry = -1;
    if (tlb >= 0) tlb_list_add(j, i, tlb);
    l->next = b->linkHead;
    b->linkHead = i;
    *patch = 0x48000000u | ((u32)off & 0x03FFFFFCu);
    if (j->flushIcache) j->flushIcache(patch, 4);
}

void h64_jit_run_one(H64System *sys)
{
    H64Jit *j = sys->jit;
    H64Cpu *cpu = &sys->cpu;
    H64JitBlock *b;
    u32 pc32, paddr;
    u32 *lastExit = j->lastExit, lastTarget = j->lastExitTarget;
    int tlbEntry = -1;   // the TLB entry pc32 goes through (-1: KSEG0/1)
    u64 flushes;

    // The shared routines (rtLink, rtIndirect) and the blocks were made for
    // one cycles-per-instruction value: the front end sets cpi after init.
    if (cpu->cpi != j->rtCpi) h64_jit_reset(sys);
    flushes = j->stats.flushes;
    lastExit = j->lastExit;
    j->lastExit = 0;
    process_events(sys);
    if (cpu->tlbGen != j->linkTlbGen)
    {
        // Only the links through the entries that changed (Conker refills
        // its TLB all the time; undoing every link each time ran it at 1 FPS).
        int e;
        if (cpu->asidGen != j->seenAsidGen) { unlink_tlb(sys, -2); j->seenAsidGen = cpu->asidGen; }
        for (e = 0; e < 32; e++)
            if (cpu->tlbEntryGen[e] != j->seenEntryGen[e])
            {
                unlink_tlb(sys, e);
                j->seenEntryGen[e] = cpu->tlbEntryGen[e];
            }
        j->linkTlbGen = cpu->tlbGen;
        lastExit = 0;
    }
    pc32 = (u32)cpu->pc;
    // Cases the recompiled code doesn't start in: delay slots, pending
    // interrupts, 64-bit addresses, misaligned pcs, code outside RDRAM.
    // KSEG0/KSEG1 (where games run) translate without the TLB.
    if (cpu->pc == (u64)(s64)(s32)pc32 && pc32 >= 0x80000000u && pc32 < 0xC0000000u)
        paddr = pc32 & 0x1FFFFFFFu;
    else if (cpu->pc != (u64)(s64)(s32)pc32)
        paddr = 0xFFFFFFFFu;
    else
    {
        // TLB-mapped: a small cache of page translations (h64_cpu_translate_debug
        // copied the whole H64Cpu at every block: Perfect Dark ran at 8 MIPS).
        u32 key = (pc32 >> 12) | (((u32)cpu->cop0[CP0_STATUS] & 0x1E) << 19);
        u32 k = (pc32 >> 12) & 255;
        int e = j->fetch[k].entry;
        if (j->fetch[k].key == key && e >= 0 && j->fetch[k].gen == cpu->tlbEntryGen[e] && j->fetch[k].asid == cpu->asidGen)
        {
            paddr = j->fetch[k].ppage | (pc32 & 0xFFFu);
            tlbEntry = e;
        }
        else if (h64_cpu_probe_fetch(cpu, pc32, &paddr, &e))
        {
            j->fetch[k].key = key;
            j->fetch[k].entry = e;
            j->fetch[k].gen = e >= 0 ? cpu->tlbEntryGen[e] : 0;
            j->fetch[k].asid = cpu->asidGen;
            j->fetch[k].ppage = paddr & ~0xFFFu;
            tlbEntry = e;
        }
        else
            paddr = 0xFFFFFFFFu;
    }
    if (cpu->branchPending || interrupt_pending(cpu) || (pc32 & 3) || paddr >= H64_RDRAM_SIZE)
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
    if (sys->sched.next < cpu->cycles + (u64)b->insns * cpu->cpi)
    {
        h64_cpu_step(sys);
        j->stats.interpSteps++;
        return;
    }
    // The previous block left by an unlinked exit to this pc: link it, so
    // that next time it jumps here directly. Native blocks only; idle loops
    // stay with the dispatcher (skipping). Links into TLB-mapped code
    // (Perfect Dark, GoldenEye, Conker) are undone whenever the TLB changes.
    if (lastExit && lastTarget == pc32 && flushes == j->stats.flushes && !j->noLink && b->kernel && b->valid && !b->idle)
        link_exit(j, lastExit, b, (pc32 >= 0x80000000u && pc32 < 0xC0000000u) ? -1 : tlbEntry);
    // Known to rtIndirect: native KSEG0/1 blocks that are not idle loops.
    if (b->kernel && b->valid && !b->idle && !j->noLink && pc32 >= 0x80000000u && pc32 < 0xC0000000u && b->body)
    {
        u32 k = (pc32 >> 2) & 1023;
        j->indPc[k] = pc32;
        j->indBody[k] = b->body;
    }
    j->blockEndCycles = cpu->cycles + (u64)b->insns * cpu->cpi;
    j->curPage = b->paddr >> 12;
    j->curInvalidated = 0;
    j->stats.blocksRun++;
    {
        u64 t0 = h64_prof_now(sys);
        j->enter(sys, b->body);
        sys->prof[H64_PROF_BLOCKS] += h64_prof_now(sys) - t0;
    }

    // Idle loop: skip whole iterations up to the next event. Nothing can
    // change the loop's outcome before an event (interrupts, DMA, the RSP
    // and the VI all arrive through the scheduler), so this equals running them.
    if (b->idle && b->valid && cpu->pc == (u64)(s64)(s32)b->vpc && !cpu->branchPending && !interrupt_pending(cpu) &&
        sys->sched.next != H64_NEVER && sys->sched.next > cpu->cycles)
    {
        u64 k = (sys->sched.next - cpu->cycles) / ((u64)b->insns * cpu->cpi);
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
            cpu->cycles += k * b->insns * cpu->cpi;
            cpu->instructions += k * b->insns;
            j->stats.idleSkipped += k * b->insns;
        }
    }
}

void h64_jit_run(H64System *sys, u64 cycles)
{
    u64 end = sys->cpu.cycles + cycles;
    sys->stop = 0;
    sys->jit->runEnd = end;
    while (sys->cpu.cycles < end && !sys->stop)
        h64_jit_run_one(sys);
    sys->jit->runEnd = ~0ull;
}
