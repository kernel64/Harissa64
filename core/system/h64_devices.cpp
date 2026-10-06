// Harissa64 V2 - RCP registers (SP, DP, MI, VI, AI, PI, RI, SI) and their
// timed side effects. Register layouts from n64brew.
//
// M1 scope: registers, DMAs and interrupts are modelled; the RSP and RDP do
// not run yet. An RSP task (SP_STATUS halt cleared) is logged from its OSTask
// header and completed after a fixed delay ("M1 RSP stub"); graphics tasks
// also raise the DP interrupt. Timings marked "model" are approximations to
// be replaced by measured values.
#include "h64_system.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"

// ---- Timing models (CPU cycles at 93.75 MHz) ----
#define CPU_HZ 93750000u
#define SI_DMA_CYCLES 4500u         // model: PIF command round trip
#define SP_TASK_CYCLES 20000u       // M1 RSP stub: delay before a task "finishes"
#define DP_AFTER_SP_CYCLES 2000u    // M1 RSP stub: RDP done after the graphics task

static const u32 VI_CLOCK_NTSC = 48681812u;
static const u32 VI_CLOCK_PAL = 49656530u;
static const u32 VI_CLOCK_MPAL = 48628316u;

static u64 now(H64System *sys) { return sys->cpu.cycles; }

// ---- MI ----
static void mi_update(H64System *sys)
{
    h64_cpu_set_ip(sys, 2, (sys->mi.intr & sys->mi.mask) != 0);
}

void h64_mi_raise(H64System *sys, u32 bits)
{
    int i;
    for (i = 0; i < 6; i++)
        if (bits & (1u << i)) sys->miRaised[i]++;
    sys->mi.intr |= bits;
    mi_update(sys);
}
void h64_mi_clear(H64System *sys, u32 bits) { sys->mi.intr &= ~bits; mi_update(sys); }

// ---- VI ----
enum { VI_STATUS = 0, VI_ORIGIN, VI_WIDTH, VI_V_INTR, VI_V_CURRENT, VI_BURST, VI_V_SYNC, VI_H_SYNC, VI_H_SYNC_LEAP,
       VI_H_VIDEO, VI_V_VIDEO, VI_V_BURST, VI_X_SCALE, VI_Y_SCALE };

static u32 vi_halflines(H64System *sys)
{
    u32 v = sys->vi.regs[VI_V_SYNC] & 0x3FF;
    return v ? v : 525;
}

// Frames advance with time only (the interrupt fires mid-frame).
static void vi_advance(H64System *sys)
{
    while (now(sys) >= sys->vi.frameStart + sys->vi.frameCycles)
    {
        sys->vi.frameStart += sys->vi.frameCycles;
        sys->vi.frames++;
    }
}

static u32 vi_current(H64System *sys)
{
    u64 into;
    vi_advance(sys);
    into = now(sys) - sys->vi.frameStart;
    u32 line = (u32)(into * vi_halflines(sys) / sys->vi.frameCycles);
    if (line >= vi_halflines(sys)) line = vi_halflines(sys) - 1;
    return line & 0x3FF;
}

static void vi_schedule(H64System *sys)
{
    u32 vintr = sys->vi.regs[VI_V_INTR] & 0x3FF;
    u64 at;
    vi_advance(sys);
    at = sys->vi.frameStart + (u64)vintr * sys->vi.frameCycles / vi_halflines(sys);
    if (vintr >= vi_halflines(sys) || at <= now(sys))
    {
        // Interrupt line not in this frame (or already passed): wake at the
        // next frame boundary to keep frames counted.
        if (vintr < vi_halflines(sys))
            at = sys->vi.frameStart + sys->vi.frameCycles + (u64)vintr * sys->vi.frameCycles / vi_halflines(sys);
        else
            at = sys->vi.frameStart + sys->vi.frameCycles;
    }
    h64_sched_set(&sys->sched, H64_EV_VI, at);
}

// ---- AI ----
static u64 ai_buffer_cycles(H64System *sys, u32 len)
{
    u32 clock = sys->tvType == 0 ? VI_CLOCK_PAL : sys->tvType == 2 ? VI_CLOCK_MPAL : VI_CLOCK_NTSC;
    u32 rate = clock / ((sys->ai.dacrate & 0x3FFF) + 1);
    if (rate == 0) rate = 1;
    return (u64)(len / 4) * CPU_HZ / rate + 1;
}

static void ai_start_next(H64System *sys)
{
    if (sys->ai.fifoCount > 0)
    {
        sys->ai.bufferCycles = ai_buffer_cycles(sys, sys->ai.fifoLen[0]);
        h64_sched_set(&sys->sched, H64_EV_AI, now(sys) + sys->ai.bufferCycles);
    }
}

// ---- SP DMA ----
static void sp_dma(H64System *sys, int toRdram, u32 value)
{
    u32 len = ((value & 0xFFF) | 7) + 1;
    u32 count = ((value >> 12) & 0xFF) + 1;
    u32 skip = (value >> 20) & 0xFF8;
    u32 mem = sys->sp.regs[0] & 0x1FF8;
    u32 dram = sys->sp.regs[1] & 0xFFFFF8;
    u32 i, j;
    for (j = 0; j < count; j++)
    {
        for (i = 0; i < len; i++)
        {
            u32 m = (mem & 0x1000) | ((mem + i) & 0xFFF);
            u32 d = dram + i;
            if (d >= H64_RDRAM_SIZE) continue;
            if (toRdram) sys->rdram[d] = sys->spMem[m];
            else sys->spMem[m] = sys->rdram[d];
        }
        mem = (mem & 0x1000) | ((mem + len) & 0xFFF);
        dram += len + skip;
    }
    sys->sp.regs[0] = mem;
    sys->sp.regs[1] = dram & 0xFFFFFF;
    sys->sp.regs[toRdram ? 3 : 2] = 0xFF8;   // length reads back as 0xFF8 after a DMA
}

static const char *task_type_name(u32 type)
{
    switch (type)
    {
    case 1: return "graphics";
    case 2: return "audio";
    case 4: return "jpeg";
    }
    return "other";
}

static void sp_start_task(H64System *sys)
{
    const u8 *t = sys->spMem + 0xFC0;   // OSTask, as libultra places it in DMEM
    u32 type = h64_load_be32(t + 0x00);
    sys->sp.tasks++;
    if (sys->sp.tasks <= 64 || (sys->sp.tasks & 1023) == 0)
        H64_INFO("[rsp] task #%u: type %u (%s), ucode %08X size %u, data %08X size %u, flags %08X (M1 stub: not run)",
                 sys->sp.tasks, type, task_type_name(type), h64_load_be32(t + 0x10), h64_load_be32(t + 0x14),
                 h64_load_be32(t + 0x30), h64_load_be32(t + 0x34), h64_load_be32(t + 0x04));
    h64_sched_set(&sys->sched, H64_EV_SP, now(sys) + SP_TASK_CYCLES);
}

static void sp_status_write(H64System *sys, u32 v)
{
    u32 s = sys->sp.regs[4];
    int wasHalted = s & SP_STATUS_HALT;
    int i;
    // Each clear/set pair: setting both bits at once changes nothing.
#define SP_PAIR(clr, set) ((v & (clr)) && !(v & (set)) ? -1 : (v & (set)) && !(v & (clr)) ? 1 : 0)
    if (SP_PAIR(0x0001, 0x0002) < 0) s &= ~SP_STATUS_HALT;
    if (SP_PAIR(0x0001, 0x0002) > 0) s |= SP_STATUS_HALT;
    if (v & 0x0004) s &= ~SP_STATUS_BROKE;
    if (SP_PAIR(0x0008, 0x0010) < 0) h64_mi_clear(sys, MI_INTR_SP);
    if (SP_PAIR(0x0008, 0x0010) > 0) h64_mi_raise(sys, MI_INTR_SP);
    if (SP_PAIR(0x0020, 0x0040) < 0) s &= ~0x0020u;
    if (SP_PAIR(0x0020, 0x0040) > 0) s |= 0x0020u;
    if (SP_PAIR(0x0080, 0x0100) < 0) s &= ~SP_STATUS_INTR_BREAK;
    if (SP_PAIR(0x0080, 0x0100) > 0) s |= SP_STATUS_INTR_BREAK;
    for (i = 0; i < 8; i++)
    {
        int p = SP_PAIR(0x0200u << (2 * i), 0x0400u << (2 * i));
        if (p < 0) s &= ~(0x0080u << i);
        if (p > 0) s |= (0x0080u << i);
    }
#undef SP_PAIR
    sys->sp.regs[4] = s;
    if (wasHalted && !(s & SP_STATUS_HALT))
        sp_start_task(sys);
}

// ---- PI DMA ----
// Block model and timing follow ares (ISC licence, ares/n64/pi/dma.cpp, see
// THIRD_PARTY.md): the PI moves cartridge -> RDRAM transfers in blocks of up
// to 128 bytes that end on a 2 KB RDRAM row, realigns the RDRAM address to
// 8 bytes after each block, and the odd first-block rules below decide how
// many bytes of a misaligned block land (PeterLemon DMAAlignment-PI-cart).

// Domain 2 covers 0x05000000-0x05FFFFFF (64DD) and 0x08000000-0x0FFFFFFF (SRAM/FlashRAM).
static const u32 *pi_bsd(H64System *sys, u32 cart)
{
    int dom2 = (cart >= 0x05000000u && cart < 0x06000000u) || (cart >= 0x08000000u && cart < 0x10000000u);
    return &sys->pi.regs[dom2 ? 9 : 5];   // latency, pulse width, page size, release
}

static u16 pi_bus_half(H64System *sys, u32 cart)
{
    if (cart >= 0x10000000u && cart - 0x10000000u + 1 < sys->rom.size)
        return h64_load_be16(sys->rom.data + (cart - 0x10000000u));
    return (u16)cart;   // nothing answers: the bus still holds the address
}

// DMA duration in CPU cycles (ares's formula in RCP cycles, x1.5).
static u64 pi_dma_cycles(H64System *sys, u32 len)
{
    const u32 *bsd = pi_bsd(sys, sys->pi.regs[1]);
    u32 pageShift = (bsd[2] & 0xF) + 2, pageSize = 1u << pageShift, pageMask = pageSize - 1;
    u32 first = sys->pi.regs[1], last = first + len - 2;
    u32 firstPage = first >> pageShift, lastPage = last >> pageShift, pages = lastPage - firstPage + 1;
    u32 buffers = 0, partial = 0;
    u64 cycles;
    if (firstPage == lastPage)
    {
        if (len == 128) buffers = 1;
        else partial = len;
    }
    else
    {
        if ((first & pageMask) == 0) buffers++;
        else partial += pageSize - (first & pageMask);
        if (((last + 2) & pageMask) == 0) buffers++;
        else partial += (last & pageMask) + 2;
        if (firstPage + 1 < lastPage)
            buffers += (pages - 2) * pageSize / 128;
    }
    cycles = (u64)(14 + bsd[0] + 1) * pages + (u64)(bsd[1] + 1 + bsd[3] + 1) * len / 2 + buffers * 28 + partial;
    return cycles * 3 / 2;
}

static void pi_dma(H64System *sys, int toRdram, u32 value)
{
    u32 len = (value & 0x00FFFFFF) + 1;
    u64 cycles = pi_dma_cycles(sys, ((len - 1) | 1) + 1);   // ares: (length | 1) + 1 bytes
    H64_DEBUG("[pi] DMA %s cart %08X dram %08X len %X", toRdram ? "cart->rdram" : "rdram->cart",
              sys->pi.regs[1], sys->pi.regs[0], len);
    if (toRdram)
    {
        u8 mem[128];
        s32 left = (s32)len, maxBlock = 128;
        int firstBlock = 1;
        u32 dram = sys->pi.regs[0], cart = sys->pi.regs[1];
        while (left > 0)
        {
            s32 misalign = (s32)(dram & 7), distEndOfRow = 0x800 - (s32)(dram & 0x7FF);
            s32 blockLen = maxBlock - misalign < distEndOfRow ? maxBlock - misalign : distEndOfRow;
            s32 curLen = left < blockLen ? left : blockLen, i;
            for (i = 0; i < curLen; i += 2)
            {
                u16 h = pi_bus_half(sys, cart);
                mem[i] = (u8)(h >> 8);
                mem[i + 1] = (u8)h;
                cart += 2;
                left -= 2;
            }
            // ares writes curLen - misalign bytes, pairwise except in a short first block.
            if (firstBlock && curLen < 127 - misalign)
                for (i = 0; i < curLen - misalign; i++, dram++)
                {
                    if (dram < H64_RDRAM_SIZE) sys->rdram[dram] = mem[i];
                }
            else
                for (i = 0; i < curLen - misalign; i += 2, dram += 2)
                {
                    if (dram < H64_RDRAM_SIZE) sys->rdram[dram] = mem[i];
                    if (dram + 1 < H64_RDRAM_SIZE) sys->rdram[dram + 1] = mem[i + 1];
                }
            dram = (dram + 7) & ~7u;
            sys->pi.regs[3] = curLen <= 8 ? (u32)(127 - misalign) : 127u;
            firstBlock = 0;
            maxBlock = distEndOfRow < 8 ? 128 - misalign : 128;
        }
        sys->pi.regs[0] = dram & 0x00FFFFFF;
        sys->pi.regs[1] = cart;
    }
    else
    {
        // RDRAM -> cartridge: only SRAM/FlashRAM would take it (M5). The
        // length register reads back rounded up to a whole halfword pair.
        sys->pi.regs[2] = ((value & 0x00FFFFFF) | 1) + 1;
    }
    sys->pi.regs[4] |= 1;   // DMA busy
    h64_sched_set(&sys->sched, H64_EV_PI, now(sys) + cycles);
}

// ---- SI DMA ----
static void si_dma(H64System *sys, int toPif)
{
    u32 dram = sys->si.dramAddr & 0x00FFFFF8;
    u32 i;
    if (toPif)
    {
        for (i = 0; i < 64; i++)
            sys->pifRam[i] = dram + i < H64_RDRAM_SIZE ? sys->rdram[dram + i] : 0;
        h64_pif_run_commands(sys);
    }
    else
    {
        for (i = 0; i < 64; i++)
            if (dram + i < H64_RDRAM_SIZE)
                sys->rdram[dram + i] = sys->pifRam[i];
    }
    sys->si.status |= 1;   // DMA busy
    h64_sched_set(&sys->sched, H64_EV_SI, now(sys) + SI_DMA_CYCLES);
}

// ---- Register access ----
u32 h64_mmio_read(H64System *sys, u32 paddr)
{
    u32 reg = (paddr & 0xFFFFF) >> 2;
    switch (paddr >> 20)
    {
    case 0x03F: return 0;                                    // RDRAM registers: not modelled (HLE boot)
    case 0x040:
        if (paddr == 0x04080000u) return sys->sp.pc & 0xFFC;
        if (paddr >= 0x04040000u && paddr < 0x04040020u)
        {
            u32 r = (paddr >> 2) & 7;
            if (r == 4) return sys->sp.regs[4];
            if (r == 5 || r == 6) return 0;                  // DMA full / busy: DMAs are instant
            if (r == 7) { u32 v = sys->sp.semaphore; sys->sp.semaphore = 1; return v; }
            return sys->sp.regs[r];
        }
        return 0;
    case 0x041: return sys->dp.regs[reg & 7];
    case 0x043:
        switch (reg & 3)
        {
        case 0: return sys->mi.mode;
        case 1: return sys->mi.version;
        case 2: return sys->mi.intr;
        default: return sys->mi.mask;
        }
    case 0x044:
        reg &= 0xF;
        if (reg == VI_V_CURRENT) return vi_current(sys);
        return reg < 14 ? sys->vi.regs[reg] : 0;
    case 0x045:
        switch (reg & 7)
        {
        case 1:   // AI_LEN: bytes left in the current buffer
            if (sys->ai.fifoCount == 0) return 0;
            {
                u64 end = sys->sched.when[H64_EV_AI];
                u64 left = end > now(sys) ? end - now(sys) : 0;
                return (u32)((u64)sys->ai.fifoLen[0] * left / (sys->ai.bufferCycles ? sys->ai.bufferCycles : 1)) & ~7u;
            }
        case 3:
            return (sys->ai.fifoCount >= 2 ? 0x80000001u : 0) | (sys->ai.fifoCount > 0 ? 0x40000000u : 0) |
                   0x00100000u;
        default: return 0;   // other AI registers are write-only
        }
    case 0x046:
        if ((reg & 0xF) == 4)   // PI_STATUS: bit 1 = IO busy while a CPU write is latched
            return sys->pi.regs[4] | (now(sys) < sys->pi.latchUntil ? 2u : 0u);
        return (reg & 0xF) < 13 ? sys->pi.regs[reg & 0xF] : 0;
    case 0x047: return sys->ri.regs[reg & 7];
    case 0x048:
        switch (reg & 7)
        {
        case 0: return sys->si.dramAddr;
        case 1: return sys->si.pifAddrRd;
        case 4: return sys->si.pifAddrWr;
        case 6: return sys->si.status | ((sys->mi.intr & MI_INTR_SI) ? 0x1000u : 0);
        }
        return 0;
    }
    return 0;
}

void h64_mmio_write(H64System *sys, u32 paddr, u32 value, u32 mask)
{
    u32 reg = (paddr & 0xFFFFF) >> 2;
    (void)mask;   // RCP registers take the full 32-bit bus value
    switch (paddr >> 20)
    {
    case 0x03F: return;
    case 0x040:
        if (paddr == 0x04080000u) { sys->sp.pc = value & 0xFFC; return; }
        if (paddr >= 0x04040000u && paddr < 0x04040020u)
        {
            u32 r = (paddr >> 2) & 7;
            switch (r)
            {
            case 0: sys->sp.regs[0] = value & 0x1FF8; return;
            case 1: sys->sp.regs[1] = value & 0xFFFFF8; return;
            case 2: sys->sp.regs[2] = value; sp_dma(sys, 0, value); return;
            case 3: sys->sp.regs[3] = value; sp_dma(sys, 1, value); return;
            case 4: sp_status_write(sys, value); return;
            case 7: sys->sp.semaphore = 0; return;
            }
        }
        return;
    case 0x041:
        switch (reg & 7)
        {
        case 0: sys->dp.regs[0] = value & 0xFFFFF8; sys->dp.regs[2] = sys->dp.regs[0]; return;
        case 1: sys->dp.regs[1] = value & 0xFFFFF8; sys->dp.regs[2] = sys->dp.regs[1]; return;   // RDP: M2
        case 3:
        {
            u32 s = sys->dp.regs[3];
            if (value & 0x01) s &= ~1u;
            if (value & 0x02) s |= 1u;
            if (value & 0x04) s &= ~2u;
            if (value & 0x08) s |= 2u;
            if (value & 0x10) s &= ~4u;
            if (value & 0x20) s |= 4u;
            sys->dp.regs[3] = s;
            return;
        }
        }
        return;
    case 0x043:
        switch (reg & 3)
        {
        case 0:
        {
            u32 m = sys->mi.mode;
            m = (m & ~0x7Fu) | (value & 0x7F);
            if (value & 0x0080) m &= ~0x080u;
            if (value & 0x0100) m |= 0x080u;
            if (value & 0x0200) m &= ~0x100u;
            if (value & 0x0400) m |= 0x100u;
            if (value & 0x0800) h64_mi_clear(sys, MI_INTR_DP);
            if (value & 0x1000) m &= ~0x200u;
            if (value & 0x2000) m |= 0x200u;
            sys->mi.mode = m;
            return;
        }
        case 3:
        {
            int i;
            for (i = 0; i < 6; i++)
            {
                if (value & (1u << (2 * i))) sys->mi.mask &= ~(1u << i);
                if (value & (2u << (2 * i))) sys->mi.mask |= (1u << i);
            }
            mi_update(sys);
            return;
        }
        }
        return;
    case 0x044:
        reg &= 0xF;
        if (reg >= 14) return;
        if (reg == VI_V_CURRENT) { h64_mi_clear(sys, MI_INTR_VI); return; }
        sys->vi.regs[reg] = value;
        if (reg == VI_V_INTR || reg == VI_V_SYNC) vi_schedule(sys);
        return;
    case 0x045:
        switch (reg & 7)
        {
        case 0: sys->ai.dramAddr = value & 0xFFFFF8; return;
        case 1:
        {
            u32 len = value & 0x3FFF8;
            if (len == 0 || sys->ai.fifoCount >= 2) return;
            sys->ai.fifoLen[sys->ai.fifoCount++] = len;
            if (sys->ai.fifoCount == 1) ai_start_next(sys);
            return;
        }
        case 2: sys->ai.control = value & 1; return;
        case 3: h64_mi_clear(sys, MI_INTR_AI); return;
        case 4: sys->ai.dacrate = value & 0x3FFF; return;
        case 5: sys->ai.bitrate = value & 0xF; return;
        }
        return;
    case 0x046:
        reg &= 0xF;
        // Only PI_STATUS can be written while a DMA or a CPU write is in flight (ares).
        if (reg != 4 && ((sys->pi.regs[4] & 1) || now(sys) < sys->pi.latchUntil))
        {
            sys->pi.regs[4] |= 4;   // error
            return;
        }
        switch (reg)
        {
        case 0: sys->pi.regs[0] = value & 0x00FFFFFE; return;
        case 1: sys->pi.regs[1] = value & ~1u; return;
        case 2: sys->pi.regs[2] = value & 0x00FFFFFF; pi_dma(sys, 0, value); return;
        case 3: sys->pi.regs[3] = value & 0x00FFFFFF; pi_dma(sys, 1, value); return;
        case 4:
            if (value & 2) { h64_mi_clear(sys, MI_INTR_PI); sys->pi.regs[4] &= ~8u; }
            if (value & 1) { sys->pi.regs[4] &= ~5u; h64_sched_cancel(&sys->sched, H64_EV_PI); }
            return;
        case 7: sys->pi.regs[7] = value & 0xF; return;   // DOM1 page size
        case 8: sys->pi.regs[8] = value & 0x3; return;   // DOM1 release
        default:
            if (reg < 13) sys->pi.regs[reg] = value & 0xFF;
            return;
        }
    case 0x047: sys->ri.regs[reg & 7] = value; return;
    case 0x048:
        switch (reg & 7)
        {
        case 0: sys->si.dramAddr = value & 0xFFFFF8; return;
        case 1: sys->si.pifAddrRd = value; si_dma(sys, 0); return;
        case 4: sys->si.pifAddrWr = value; si_dma(sys, 1); return;
        case 6: h64_mi_clear(sys, MI_INTR_SI); return;
        }
        return;
    }
}

// ---- Timed events ----
void h64_device_event(H64System *sys, int ev)
{
    switch (ev)
    {
    case H64_EV_VI:
        if ((sys->vi.regs[VI_V_INTR] & 0x3FF) < vi_halflines(sys))
            h64_mi_raise(sys, MI_INTR_VI);
        vi_schedule(sys);
        break;
    case H64_EV_AI:
        if (sys->ai.fifoCount > 0)
        {
            sys->ai.fifoLen[0] = sys->ai.fifoLen[1];
            sys->ai.fifoCount--;
            h64_mi_raise(sys, MI_INTR_AI);
            ai_start_next(sys);
        }
        break;
    case H64_EV_PI:
        sys->pi.regs[4] = (sys->pi.regs[4] & ~3u) | 8u;
        h64_mi_raise(sys, MI_INTR_PI);
        break;
    case H64_EV_SI:
        sys->si.status &= ~1u;
        h64_mi_raise(sys, MI_INTR_SI);
        break;
    case H64_EV_SP:
    {
        u32 type = h64_load_be32(sys->spMem + 0xFC0);
        // A finished task leaves SIG2 ("task done") set, then halts on BREAK.
        sys->sp.regs[4] |= SP_STATUS_HALT | SP_STATUS_BROKE | 0x0200u;
        if (sys->sp.regs[4] & SP_STATUS_INTR_BREAK)
            h64_mi_raise(sys, MI_INTR_SP);
        if (type == 1)
            h64_sched_set(&sys->sched, H64_EV_DP, now(sys) + DP_AFTER_SP_CYCLES);
        break;
    }
    case H64_EV_DP:
        h64_mi_raise(sys, MI_INTR_DP);
        break;
    case H64_EV_COMPARE:
        h64_cpu_set_ip(sys, 7, 1);
        h64_cpu_reschedule_compare(sys);
        break;
    }
}

void h64_devices_reset(H64System *sys)
{
    memset(&sys->mi, 0, sizeof(sys->mi));
    memset(&sys->vi, 0, sizeof(sys->vi));
    memset(&sys->ai, 0, sizeof(sys->ai));
    memset(&sys->pi, 0, sizeof(sys->pi));
    memset(&sys->ri, 0, sizeof(sys->ri));
    memset(&sys->si, 0, sizeof(sys->si));
    memset(&sys->sp, 0, sizeof(sys->sp));
    memset(&sys->dp, 0, sizeof(sys->dp));
    sys->mi.version = 0x02020102u;
    sys->sp.regs[4] = SP_STATUS_HALT;
    sys->dp.regs[3] = 0x80;          // DPC_STATUS: CBUF ready
    sys->vi.regs[VI_V_SYNC] = sys->tvType == 0 ? 625 : 525;
    sys->vi.regs[VI_V_INTR] = 0x3FF;
    sys->vi.frameCycles = sys->tvType == 0 ? CPU_HZ / 50 : CPU_HZ / 60;
    sys->vi.frameStart = now(sys);
    vi_schedule(sys);
}
