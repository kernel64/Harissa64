// Harissa64 V2 - RDP command interface (DPC registers and command fetch).
// Register behaviour from n64brew ("Reality Display Processor/Interface").
#include "h64_rdp.h"
#include "h64_rdp_state.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"
#include "../../render/api.h"

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

// Runs the commands between DPC_CURRENT and DPC_END.
static void run_commands(H64System *sys)
{
    H64RdpRegs *dp = &sys->dp;
    if (dp->status & DPC_FREEZE) return;
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
        if (v & 0x004) { dp->status &= ~DPC_FREEZE; run_commands(sys); }
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
