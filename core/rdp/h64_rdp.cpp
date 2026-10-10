// Harissa64 V2 - RDP command interface (DPC registers and command fetch).
// Register behaviour from n64brew ("Reality Display Processor/Interface").
#include "h64_rdp.h"
#include "h64_rdp_state.h"

#include <stdlib.h>
#include <string.h>

#include <vector>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../common/h64_mem.h"
#include "../system/h64_system.h"
#include "../../render/api.h"
#include "../hle/h64_hle.h"

// Command length in 64-bit words, from the opcode (bits 56..61).
static u32 command_words(u32 opcode)
{
    if (opcode >= 0x08 && opcode <= 0x0F)
        return 4 + ((opcode & 4) ? 8 : 0) + ((opcode & 2) ? 8 : 0) + ((opcode & 1) ? 2 : 0);
    if (opcode == 0x24 || opcode == 0x25) return 2;   // texture rectangle (flip)
    return 1;
}

void h64_rdp_reset(H64System *sys)
{
    memset(&sys->dp, 0, sizeof(sys->dp));
    sys->dp.status = DPC_CBUF_READY;
    sys->dpPendingWords = 0;
    if (sys->rdpState)
    {
        memset(sys->rdpState, 0, sizeof(*sys->rdpState));
        sys->rdpState->colorFmt = FB_RGBA5551;
        sys->rdpState->colorWidth = 320;
    }
    if (sys->rdramHidden) memset(sys->rdramHidden, 3, H64_RDRAM_SIZE / 2);
}

static u64 fetch_word(H64System *sys, u32 addr)
{
    if (sys->dp.status & DPC_XBUS)
        return h64_load_be64(sys->spMem + (addr & 0xFF8));
    addr &= 0xFFFFF8;
    return addr + 8 <= H64_RDRAM_SIZE ? h64_load_be64(sys->rdram + addr) : 0;
}

// RDP commands the CPU (or an LLE RSP task) sends through DPC_END while the
// graphics worker owns the renderer: they run there too, in order, instead
// of waiting for it. Perfect Dark's cutscenes send their motion blur this
// way, a few times a frame: the CPU waited ~13 ms a frame for the worker.
// Texture loads read a snapshot of what they load, taken when the commands
// are sent (the CPU goes on writing RDRAM meanwhile); commands that load
// from a texture image set before them (not known here) run at once, after
// waiting for the worker, as before.
struct H64DpcBatch
{
    H64System *sys;
    std::vector<u64> words;    // each command: its word count, then its words
    u8 *snap;                  // RDRAM-sized; the loaded ranges are valid
    u32 ticket;
    int queued;
};

struct H64DpcAsync
{
    H64DpcBatch batch[2];
    u32 next;
    std::vector<u32> ranges;   // start, end pairs to copy into the snapshot
};

void h64_rdp_async_free(H64System *sys)
{
    if (!sys->dpcAsync) return;   // the caller waited for the worker
    h64_big_free(sys->dpcAsync->batch[0].snap);
    h64_big_free(sys->dpcAsync->batch[1].snap);
    delete sys->dpcAsync;
    sys->dpcAsync = 0;
}

static void run_batch(H64System *sys, const std::vector<u64> &words)
{
    size_t i = 0;
    while (i < words.size())
    {
        u32 n = (u32)words[i];
        u64 t0 = h64_prof_now(sys);
        if (sys->renderer) sys->renderer->rdp(sys->renderer->user, &words[i + 1], n);
        else h64_rdp_command(sys, &words[i + 1], n);
        sys->prof[H64_PROF_RENDER] += h64_prof_now(sys) - t0;
        i += 1 + n;
    }
}

static void dpc_job(void *arg)
{
    H64DpcBatch *b = (H64DpcBatch *)arg;
    H64RdpState *st = h64_rdp_state(b->sys);
    st->loadRam = b->snap;
    st->loadRamGen++;
    run_batch(b->sys, b->words);
    st->loadRam = 0;
}

// The RDRAM a texture load reads (generously rounded), from the texture image.
static void load_range(std::vector<u32> &r, u32 op, const u64 *w, u32 addr, u32 width, u32 size)
{
    u32 w0 = (u32)(w[0] >> 32), w1 = (u32)w[0];
    u32 sl = (w0 >> 12) & 0xFFF, tl = w0 & 0xFFF, sh = (w1 >> 12) & 0xFFF, th = w1 & 0xFFF;
    u32 bpl = (width << size) >> 1, lo, hi;   // bytes per image line
    if (op == 0x33)   // LOAD_BLOCK: texels from (sl, tl) on
    {
        lo = addr + tl * bpl + ((sl << size) >> 1);
        hi = lo + ((((sh - sl + 1) & 0xFFF) << size) >> 1) + 16;
    }
    else              // LOAD_TILE, LOAD_TLUT: lines tl..th
    {
        lo = addr + (tl >> 2) * bpl;
        hi = addr + ((th >> 2) + 1) * bpl + 16;
    }
    lo &= 0xFFFFF8;
    if (hi > H64_RDRAM_SIZE) hi = H64_RDRAM_SIZE;
    if (lo < hi) { r.push_back(lo); r.push_back(hi); }
}

static void run_commands_async(H64System *sys)
{
    H64RdpRegs *dp = &sys->dp;
    H64DpcAsync *a = sys->dpcAsync;
    H64DpcBatch *b;
    u32 timgAddr = 0, timgWidth = 0, timgSize = 0, k;
    int timgKnown = 0, sync = 0;
    if (!a)
    {
        a = sys->dpcAsync = new H64DpcAsync;
        a->next = 0;
        for (k = 0; k < 2; k++) { a->batch[k].sys = sys; a->batch[k].snap = 0; a->batch[k].queued = 0; }
    }
    b = &a->batch[a->next & 1];
    if (b->queued)
    {
        // Its previous run may still read the words and the snapshot.
        u64 t0 = h64_prof_now(sys);
        sys->asyncWaitTicket(sys->asyncUser, b->ticket);
        sys->prof[H64_PROF_WAIT_DPC] += h64_prof_now(sys) - t0;
        b->queued = 0;
    }
    b->words.clear();
    a->ranges.clear();
    dp->status |= DPC_PIPE_BUSY | DPC_START_GCLK;
    while (dp->current < dp->end)
    {
        u64 w = fetch_word(sys, dp->current);
        dp->current += 8;
        sys->dpCommand[sys->dpPendingWords++] = w;
        if (sys->dpPendingWords >= command_words((u32)(sys->dpCommand[0] >> 56) & 0x3F))
        {
            u32 opcode = (u32)(sys->dpCommand[0] >> 56) & 0x3F, i;
            b->words.push_back(sys->dpPendingWords);
            for (i = 0; i < sys->dpPendingWords; i++) b->words.push_back(sys->dpCommand[i]);
            if (opcode == 0x3D)
            {
                u32 w0 = (u32)(sys->dpCommand[0] >> 32);
                timgAddr = (u32)sys->dpCommand[0] & 0xFFFFFF;
                timgWidth = (w0 & 0x3FF) + 1;
                timgSize = (w0 >> 19) & 3;
                timgKnown = 1;
            }
            else if (opcode == 0x30 || opcode == 0x33 || opcode == 0x34)
            {
                if (timgKnown) load_range(a->ranges, opcode, sys->dpCommand, timgAddr, timgWidth, timgSize);
                else sync = 1;
            }
            sys->dpPendingWords = 0;
            sys->dpCommands++;
            if (opcode == 0x29)   // SYNC_FULL: the RDP is idle, interrupt the CPU
            {
                dp->status &= ~(DPC_PIPE_BUSY | DPC_START_GCLK | DPC_CMD_BUSY | DPC_TMEM_BUSY);
                h64_mi_raise(sys, MI_INTR_DP);
            }
        }
    }
    if (b->words.empty()) return;
    if (!sync && !b->snap) b->snap = (u8 *)h64_big_alloc(H64_RDRAM_SIZE);
    if (sync || !b->snap)
    {
        u64 t0 = h64_prof_now(sys);
        h64_hle_async_wait(sys);
        sys->prof[H64_PROF_WAIT_DPC] += h64_prof_now(sys) - t0;
        run_batch(sys, b->words);
        return;
    }
    for (k = 0; k + 1 < a->ranges.size(); k += 2)
        memcpy(b->snap + a->ranges[k], sys->rdram + a->ranges[k], a->ranges[k + 1] - a->ranges[k]);
    b->ticket = sys->asyncStart(sys->asyncUser, dpc_job, b);
    b->queued = 1;
    a->next++;
}

// Runs the commands between DPC_CURRENT and DPC_END.
static void run_commands(H64System *sys)
{
    H64RdpRegs *dp = &sys->dp;
    if (dp->status & DPC_FREEZE) return;
    if (sys->asyncStart && sys->asyncWaitTicket && !sys->options.noRdpDraw)
    {
        run_commands_async(sys);
        return;
    }
    {
        u64 t0 = h64_prof_now(sys);
        h64_hle_async_wait(sys);   // the renderer belongs to the graphics worker while a task runs
        sys->prof[H64_PROF_WAIT_DPC] += h64_prof_now(sys) - t0;
    }
    // Once given commands the RDP's pipeline runs (clock started) until a
    // SYNC_FULL (n64-systemtest "RDP STATUS: Flags during a run").
    dp->status |= DPC_PIPE_BUSY | DPC_START_GCLK;
    while (dp->current < dp->end)
    {
        u64 w = fetch_word(sys, dp->current);
        dp->current += 8;
        sys->dpCommand[sys->dpPendingWords++] = w;
        if (sys->dpPendingWords >= command_words((u32)(sys->dpCommand[0] >> 56) & 0x3F))
        {
            u32 opcode = (u32)(sys->dpCommand[0] >> 56) & 0x3F;
            u64 t0 = h64_prof_now(sys);
            if (sys->renderer) sys->renderer->rdp(sys->renderer->user, sys->dpCommand, sys->dpPendingWords);
            else h64_rdp_command(sys, sys->dpCommand, sys->dpPendingWords);
            sys->prof[H64_PROF_RENDER] += h64_prof_now(sys) - t0;
            sys->dpPendingWords = 0;
            sys->dpCommands++;
            if (opcode == 0x29)   // SYNC_FULL: the RDP is idle, interrupt the CPU
            {
                dp->status &= ~(DPC_PIPE_BUSY | DPC_START_GCLK | DPC_CMD_BUSY | DPC_TMEM_BUSY);
                h64_mi_raise(sys, MI_INTR_DP);
            }
        }
    }
}

// The graphics HLE draws its whole task at once, but the commands it stands
// for go through the RDP, which processes nothing while DPC_STATUS.FREEZE is
// set: their SYNC_FULL, and so the DP interrupt, comes only once the CPU
// clears the freeze. DK64 and Banjo-Tooie freeze the RDP after each DP
// interrupt and unfreeze it at the next VI: an interrupt raised while frozen
// let DK64's intro run twice as fast as on the console (its EEPROM thread was
// then restarted while asleep) and was lost by Banjo-Tooie's scheduler (stuck
// on its loading screen).
void h64_rdp_hle_full_sync(H64System *sys)
{
    if (sys->dp.status & DPC_FREEZE)
        sys->dp.hleSyncPending = 1;
    else
        h64_mi_raise(sys, MI_INTR_DP);
}

u32 h64_dp_read(H64System *sys, u32 reg)
{
    H64RdpRegs *dp = &sys->dp;
    switch (reg & 7)
    {
    case 0: return dp->start;
    case 1: return dp->end;
    case 2: return dp->current;
    case 3: return dp->status;
    case 4: return dp->clock & 0xFFFFFF;
    case 5: return dp->bufBusy & 0xFFFFFF;
    case 6: return dp->pipeBusy & 0xFFFFFF;
    default: return dp->tmemCounter & 0xFFFFFF;
    }
}

void h64_dp_write(H64System *sys, u32 reg, u32 v)
{
    H64RdpRegs *dp = &sys->dp;
    switch (reg & 7)
    {
    case 0:   // DPC_START: taken only when no start is already pending
        if (!(dp->status & DPC_START_VALID))
        {
            dp->start = v & 0xFFFFF8;
            dp->status |= DPC_START_VALID;
        }
        return;
    case 1:   // DPC_END: a pending start resets the current pointer
        dp->end = v & 0xFFFFF8;
        if (dp->status & DPC_START_VALID)
        {
            dp->current = dp->start;
            dp->status &= ~DPC_START_VALID;
        }
        run_commands(sys);
        return;
    case 3:
        if (v & 0x001) dp->status &= ~DPC_XBUS;
        if (v & 0x002) dp->status |= DPC_XBUS;
        if (v & 0x004)
        {
            dp->status &= ~DPC_FREEZE;
            run_commands(sys);
            if (dp->hleSyncPending)
            {
                // The HLE task's commands come before anything queued since.
                dp->hleSyncPending = 0;
                h64_mi_raise(sys, MI_INTR_DP);
            }
        }
        if (v & 0x008) dp->status |= DPC_FREEZE;
        if (v & 0x010) dp->status &= ~DPC_FLUSH;
        if (v & 0x020) dp->status |= DPC_FLUSH;
        if (v & 0x040) dp->tmemCounter = 0;
        if (v & 0x080) dp->pipeBusy = 0;
        if (v & 0x100) dp->bufBusy = 0;
        if (v & 0x200) dp->clock = 0;
        return;
    }
}
