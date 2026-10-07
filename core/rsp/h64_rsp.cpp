// Harissa64 V2 - RSP scalar unit, SP registers and SP DMA.
// Ported from ares (ISC licence): ares/n64/rsp/interpreter.cpp,
// interpreter-ipu.cpp, interpreter-scc.cpp, dma.cpp, io.cpp (commit a776c509).
#include "h64_rsp.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../hle/h64_hle.h"
#include "../rdp/h64_rdp.h"
#include "../system/h64_system.h"

// SP_STATUS bits
#define ST_HALT     0x0001u
#define ST_BROKE    0x0002u
#define ST_DMABUSY  0x0004u
#define ST_DMAFULL  0x0008u
#define ST_IOFULL   0x0010u
#define ST_SSTEP    0x0020u
#define ST_INTBREAK 0x0040u

// ---- DMEM/IMEM ----
static u8 *dmem(H64System *sys) { return sys->spMem; }
static u8 *imem(H64System *sys) { return sys->spMem + 0x1000; }

static u8 dmem_r8(H64System *sys, u32 a) { return dmem(sys)[a & 0xFFF]; }
static void dmem_w8(H64System *sys, u32 a, u8 v) { dmem(sys)[a & 0xFFF] = v; }
// Unaligned accesses wrap around DMEM byte by byte.
static u32 dmem_r16u(H64System *sys, u32 a) { return (u32)dmem_r8(sys, a) << 8 | dmem_r8(sys, a + 1); }
static u32 dmem_r32u(H64System *sys, u32 a) { return dmem_r16u(sys, a) << 16 | dmem_r16u(sys, a + 2); }
static void dmem_w16u(H64System *sys, u32 a, u32 v) { dmem_w8(sys, a, (u8)(v >> 8)); dmem_w8(sys, a + 1, (u8)v); }
static void dmem_w32u(H64System *sys, u32 a, u32 v) { dmem_w16u(sys, a, v >> 16); dmem_w16u(sys, a + 2, v); }

// ---- Reset ----
void h64_rsp_reset(H64System *sys)
{
    H64Rsp *rsp = &sys->rsp;
    memset(rsp, 0, sizeof(*rsp));
    rsp->status = ST_HALT;
    rsp->nextPc = 4;
    rsp->current.length = 0xFF8;
    rsp->pending.length = 0;
    h64_rsp_vu_init_tables(rsp);
}

// ---- SP DMA (timed: one RCP cycle per 8 bytes, ares) ----
static u64 dma_cycles(const H64SpDma *d)
{
    u32 rcp = (d->length + 8) / 8;
    return (u64)rcp * 3 / 2 + 1;
}

static void dma_start(H64System *sys)
{
    H64Rsp *rsp = &sys->rsp;
    if (rsp->dmaBusy || !rsp->dmaFull) return;
    rsp->current = rsp->pending;
    rsp->dmaBusy = 1;
    rsp->dmaFull = 0;
    h64_sched_set(&sys->sched, H64_EV_SP, sys->cpu.cycles + dma_cycles(&rsp->current));
}

void h64_sp_dma_event(H64System *sys)
{
    H64Rsp *rsp = &sys->rsp;
    H64SpDma *d = &rsp->current;
    u32 i;
    if (!rsp->dmaBusy) return;
    if (d->toRdram) h64_jit_notify_write(sys, d->dramAddr & 0xFFFFF8, d->length + 8);
    for (i = 0; i <= d->length; i += 8)
    {
        u32 m = (d->memAddr & 0x1000) | (d->memAddr & 0xFF8);
        u32 dram = d->dramAddr & 0xFFFFF8;
        u32 k;
        for (k = 0; k < 8; k++)
        {
            u32 dk = dram + k;
            if (d->toRdram) { if (dk < H64_RDRAM_SIZE) sys->rdram[dk] = sys->spMem[m + k]; }
            else sys->spMem[m + k] = dk < H64_RDRAM_SIZE ? sys->rdram[dk] : 0;
        }
        d->dramAddr = (d->dramAddr + 8) & 0xFFFFF8;
        d->memAddr = (d->memAddr & 0x1000) | ((d->memAddr + 8) & 0xFF8);
    }
    if (d->count)
    {
        d->count--;
        d->dramAddr = (d->dramAddr + d->skip) & 0xFFFFF8;
        h64_sched_set(&sys->sched, H64_EV_SP, sys->cpu.cycles + dma_cycles(d));
    }
    else
    {
        rsp->dmaBusy = 0;
        d->length = 0xFF8;
        dma_start(sys);
    }
}

static void dma_request(H64System *sys, u32 value, int toRdram)
{
    H64Rsp *rsp = &sys->rsp;
    rsp->pending.length = value & 0xFF8;
    rsp->pending.count = (value >> 12) & 0xFF;
    rsp->pending.skip = (value >> 20) & 0xFF8;
    rsp->pending.toRdram = toRdram;
    rsp->dmaFull = 1;
    dma_start(sys);
}

// ---- SP registers ----
static void log_task(H64System *sys)
{
    const u8 *t = sys->spMem + 0xFC0;   // OSTask, as libultra places it in DMEM
    sys->rsp.tasks++;
    if (sys->rsp.tasks <= 16 || (sys->rsp.tasks & 1023) == 0)
        H64_DEBUG("[rsp] start #%u at pc %03X: OSTask type %u, ucode %08X, data %08X size %u", sys->rsp.tasks,
                  sys->rsp.pc, h64_load_be32(t), h64_load_be32(t + 0x10), h64_load_be32(t + 0x30),
                  h64_load_be32(t + 0x34));
}

u32 h64_sp_read(H64System *sys, u32 reg)
{
    H64Rsp *rsp = &sys->rsp;
    switch (reg & 7)
    {
    case 0: return rsp->current.memAddr & 0x1FFF;
    case 1: return rsp->current.dramAddr & 0xFFFFFF;
    case 2: case 3: return rsp->current.length | rsp->current.count << 12 | rsp->current.skip << 20;
    case 4:
        return rsp->status | (rsp->dmaBusy ? ST_DMABUSY : 0) | (rsp->dmaFull ? ST_DMAFULL : 0);
    case 5: return rsp->dmaFull ? 1 : 0;
    case 6: return rsp->dmaBusy ? 1 : 0;
    default:
    {
        u32 v = rsp->semaphore;
        rsp->semaphore = 1;
        return v;
    }
    }
}

void h64_sp_write(H64System *sys, u32 reg, u32 v)
{
    H64Rsp *rsp = &sys->rsp;
    switch (reg & 7)
    {
    case 0: rsp->pending.memAddr = v & 0x1FF8; return;
    case 1: rsp->pending.dramAddr = v & 0xFFFFF8; return;
    case 2: dma_request(sys, v, 0); return;
    case 3: dma_request(sys, v, 1); return;
    case 4:
    {
        u32 s = rsp->status;
        int wasHalted = (s & ST_HALT) != 0, i;
        // Each clear/set pair: setting both bits at once changes nothing.
#define SP_PAIR(clr, set) ((v & (clr)) && !(v & (set)) ? -1 : (v & (set)) && !(v & (clr)) ? 1 : 0)
        if (SP_PAIR(0x0001, 0x0002) < 0) s &= ~ST_HALT;
        if (SP_PAIR(0x0001, 0x0002) > 0) s |= ST_HALT;
        if (v & 0x0004) s &= ~ST_BROKE;
        if (SP_PAIR(0x0008, 0x0010) < 0) h64_mi_clear(sys, MI_INTR_SP);
        if (SP_PAIR(0x0008, 0x0010) > 0) h64_mi_raise(sys, MI_INTR_SP);
        if (SP_PAIR(0x0020, 0x0040) < 0) s &= ~ST_SSTEP;
        if (SP_PAIR(0x0020, 0x0040) > 0) s |= ST_SSTEP;
        if (SP_PAIR(0x0080, 0x0100) < 0) s &= ~ST_INTBREAK;
        if (SP_PAIR(0x0080, 0x0100) > 0) s |= ST_INTBREAK;
        for (i = 0; i < 8; i++)
        {
            int p = SP_PAIR(0x0200u << (2 * i), 0x0400u << (2 * i));
            if (p < 0) s &= ~(0x0080u << i);
            if (p > 0) s |= (0x0080u << i);
        }
#undef SP_PAIR
        rsp->status = s;
        if (wasHalted && !(s & ST_HALT))
        {
            u32 busy = 0;
            rsp->cycleFrac = 0;
            rsp->syncedCycles = sys->cpu.cycles;
            log_task(sys);
            if (h64_hle_try_task(sys, &rsp->hleStatus, &busy, &rsp->hleDpInterrupt))
            {
                // The HLE ran the whole task; the RSP looks busy until then.
                rsp->hleBusy = 1;
                rsp->hleTasks++;
                h64_sched_set(&sys->sched, H64_EV_RSP, sys->cpu.cycles + busy);
            }
            else
                h64_sched_set(&sys->sched, H64_EV_RSP, sys->cpu.cycles + H64_RSP_SLICE);
        }
        return;
    }
    case 5: case 6: return;   // read-only
    default: rsp->semaphore = 0; return;
    }
}

u32 h64_sp_pc_read(H64System *sys) { return sys->rsp.pc & 0xFFF; }

void h64_sp_pc_write(H64System *sys, u32 value)
{
    sys->rsp.pc = value & 0xFFC;
    sys->rsp.nextPc = (sys->rsp.pc + 4) & 0xFFF;
}

// ---- Scalar unit ----
#define OP_RS(op) (((op) >> 21) & 31)
#define OP_RT(op) (((op) >> 16) & 31)
#define OP_RD(op) (((op) >> 11) & 31)
#define OP_SA(op) (((op) >> 6) & 31)
#define IMM16(op) ((u32)(s32)(s16)(op))

static void take_branch(H64Rsp *rsp, u32 target) { rsp->nextPc = target & 0xFFF; }

static void do_break(H64System *sys)
{
    sys->rsp.status |= ST_HALT | ST_BROKE;
    if (sys->rsp.status & ST_INTBREAK)
        h64_mi_raise(sys, MI_INTR_SP);
}

// COP0: SP registers (0..7) and DP registers (8..15).
static u32 cop0_read(H64System *sys, u32 rd)
{
    if (rd & 8) return h64_dp_read(sys, rd & 7);
    return h64_sp_read(sys, rd & 7);
}

static void cop0_write(H64System *sys, u32 rd, u32 v)
{
    if (rd & 8) h64_dp_write(sys, rd & 7, v);
    else h64_sp_write(sys, rd & 7, v);
}

static void execute(H64System *sys, u32 op, u32 pc)
{
    H64Rsp *rsp = &sys->rsp;
    u32 *r = rsp->r;
    u32 rs = r[OP_RS(op)], rt = r[OP_RT(op)];
    int t = OP_RT(op), d = OP_RD(op);
    switch (op >> 26)
    {
    case 0x00:   // SPECIAL
        switch (op & 0x3F)
        {
        case 0x00: r[d] = rt << OP_SA(op); break;                          // SLL
        case 0x02: r[d] = rt >> OP_SA(op); break;                          // SRL
        case 0x03: r[d] = (u32)((s32)rt >> OP_SA(op)); break;              // SRA
        case 0x04: r[d] = rt << (rs & 31); break;                          // SLLV
        case 0x06: r[d] = rt >> (rs & 31); break;                          // SRLV
        case 0x07: r[d] = (u32)((s32)rt >> (rs & 31)); break;              // SRAV
        case 0x08: take_branch(rsp, rs); break;                            // JR
        case 0x09: take_branch(rsp, rs); r[d] = (pc + 8) & 0xFFF; break;   // JALR
        case 0x0D: do_break(sys); break;                                   // BREAK
        case 0x20: case 0x21: r[d] = rs + rt; break;                       // ADD(U)
        case 0x22: case 0x23: r[d] = rs - rt; break;                       // SUB(U)
        case 0x24: r[d] = rs & rt; break;
        case 0x25: r[d] = rs | rt; break;
        case 0x26: r[d] = rs ^ rt; break;
        case 0x27: r[d] = ~(rs | rt); break;
        case 0x2A: r[d] = (s32)rs < (s32)rt ? 1 : 0; break;
        case 0x2B: r[d] = rs < rt ? 1 : 0; break;
        default: r[d] = rs >> (rs & 31); break;   // other encodings behave as SRLV rd, rs, rs (ares)
        }
        break;
    case 0x01:   // REGIMM
        switch (t)
        {
        case 0x00: if ((s32)rs < 0) take_branch(rsp, pc + 4 + (IMM16(op) << 2)); break;    // BLTZ
        case 0x01: if ((s32)rs >= 0) take_branch(rsp, pc + 4 + (IMM16(op) << 2)); break;   // BGEZ
        case 0x10: if ((s32)rs < 0) take_branch(rsp, pc + 4 + (IMM16(op) << 2)); r[31] = (pc + 8) & 0xFFF; break;
        case 0x11: if ((s32)rs >= 0) take_branch(rsp, pc + 4 + (IMM16(op) << 2)); r[31] = (pc + 8) & 0xFFF; break;
        }
        break;
    case 0x02: take_branch(rsp, (op & 0x3FFFFFF) << 2); break;                                  // J
    case 0x03: r[31] = (pc + 8) & 0xFFF; take_branch(rsp, (op & 0x3FFFFFF) << 2); break;       // JAL
    case 0x04: if (rs == rt) take_branch(rsp, pc + 4 + (IMM16(op) << 2)); break;               // BEQ
    case 0x05: if (rs != rt) take_branch(rsp, pc + 4 + (IMM16(op) << 2)); break;               // BNE
    case 0x06: if ((s32)rs <= 0) take_branch(rsp, pc + 4 + (IMM16(op) << 2)); break;           // BLEZ
    case 0x07: if ((s32)rs > 0) take_branch(rsp, pc + 4 + (IMM16(op) << 2)); break;            // BGTZ
    case 0x08: case 0x09: r[t] = rs + IMM16(op); break;                                         // ADDI(U)
    case 0x0A: r[t] = (s32)rs < (s32)IMM16(op) ? 1 : 0; break;                                  // SLTI
    case 0x0B: r[t] = rs < IMM16(op) ? 1 : 0; break;                                            // SLTIU
    case 0x0C: r[t] = rs & (op & 0xFFFF); break;
    case 0x0D: r[t] = rs | (op & 0xFFFF); break;
    case 0x0E: r[t] = rs ^ (op & 0xFFFF); break;
    case 0x0F: r[t] = (op & 0xFFFF) << 16; break;                                               // LUI
    case 0x10:   // COP0
        switch (OP_RS(op))
        {
        case 0x00: { u32 v = cop0_read(sys, d); r[t] = v; break; }   // MFC0
        case 0x04: cop0_write(sys, d, rt); break;                    // MTC0
        }
        break;
    case 0x12: h64_rsp_cop2(sys, op); break;
    case 0x20: r[t] = (u32)(s32)(s8)dmem_r8(sys, rs + IMM16(op)); break;          // LB
    case 0x21: r[t] = (u32)(s32)(s16)dmem_r16u(sys, rs + IMM16(op)); break;       // LH
    case 0x23: case 0x27: r[t] = dmem_r32u(sys, rs + IMM16(op)); break;           // LW, LWU
    case 0x24: r[t] = dmem_r8(sys, rs + IMM16(op)); break;                        // LBU
    case 0x25: r[t] = dmem_r16u(sys, rs + IMM16(op)); break;                      // LHU
    case 0x28: dmem_w8(sys, rs + IMM16(op), (u8)rt); break;                       // SB
    case 0x29: dmem_w16u(sys, rs + IMM16(op), rt); break;                         // SH
    case 0x2B: dmem_w32u(sys, rs + IMM16(op), rt); break;                         // SW
    case 0x32: h64_rsp_lwc2(sys, op); break;
    case 0x3A: h64_rsp_swc2(sys, op); break;
    default: break;   // invalid encodings do nothing
    }
    r[0] = 0;
}

void h64_rsp_step(H64System *sys)
{
    H64Rsp *rsp = &sys->rsp;
    u32 pc = rsp->pc;
    u32 op = h64_load_be32(imem(sys) + (pc & 0xFFC));
    rsp->pc = rsp->nextPc;
    rsp->nextPc = (rsp->nextPc + 4) & 0xFFF;
    execute(sys, op, pc);
    rsp->instructions++;
    if (rsp->status & ST_SSTEP)
        rsp->status |= ST_HALT;
}

void h64_rsp_advance(H64System *sys, u32 cpuCycles)
{
    H64Rsp *rsp = &sys->rsp;
    if ((rsp->status & ST_HALT) || rsp->hleBusy) return;
    // 2 RCP cycles per 3 CPU cycles.
    rsp->cycleFrac += cpuCycles * 2;
    while (rsp->cycleFrac >= 3)
    {
        rsp->cycleFrac -= 3;
        h64_rsp_step(sys);
        if (rsp->status & ST_HALT)
        {
            rsp->cycleFrac = 0;
            if (sys->options.hleAudioCheck) h64_hle_check_end(sys);
            return;
        }
    }
}

void h64_rsp_sync(H64System *sys)
{
    H64Rsp *rsp = &sys->rsp;
    u64 now = sys->cpu.cycles, delta;
    if (rsp->inSync || now <= rsp->syncedCycles) return;
    delta = now - rsp->syncedCycles;
    rsp->syncedCycles = now;
    if (rsp->status & ST_HALT) return;
    rsp->inSync = 1;
    {
        u64 t0 = h64_prof_now(sys);
        while (delta && !(rsp->status & ST_HALT))
        {
            u32 c = delta > 0x10000000u ? 0x10000000u : (u32)delta;
            h64_rsp_advance(sys, c);
            delta -= c;
        }
        sys->prof[H64_PROF_RSP_LLE] += h64_prof_now(sys) - t0;
    }
    rsp->inSync = 0;
}

void h64_rsp_slice_event(H64System *sys)
{
    H64Rsp *rsp = &sys->rsp;
    if (rsp->hleBusy)
    {
        int ran, fullSync, kind;
        u32 bits = 0;
        kind = h64_hle_async_finish(sys, &ran, &fullSync, &bits);
        if (kind)
        {
            if (!ran)
            {
                // The graphics HLE fell back (G_LOAD_UCODE to an unknown
                // microcode): the LLE RSP runs the task from now.
                rsp->hleBusy = 0;
                rsp->syncedCycles = sys->cpu.cycles;
                rsp->cycleFrac = 0;
                h64_sched_set(&sys->sched, H64_EV_RSP, sys->cpu.cycles + H64_RSP_SLICE);
                return;
            }
            rsp->hleDpInterrupt = fullSync;
            if (kind == 2) rsp->hleStatus = bits;
        }
        // End of an HLE task: halt with the bits the microcode would set.
        rsp->hleBusy = 0;
        rsp->status |= rsp->hleStatus | ST_HALT;
        if ((rsp->hleStatus & ST_BROKE) && (rsp->status & ST_INTBREAK))
            h64_mi_raise(sys, MI_INTR_SP);
        if (rsp->hleDpInterrupt)
        {
            rsp->hleDpInterrupt = 0;
            h64_mi_raise(sys, MI_INTR_DP);
        }
        return;
    }
    h64_rsp_sync(sys);
    if (!(sys->rsp.status & ST_HALT))
        h64_sched_set(&sys->sched, H64_EV_RSP, sys->cpu.cycles + H64_RSP_SLICE);
}
