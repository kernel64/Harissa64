// Harissa64 V2 - VR4300 reference interpreter: integer unit, COP0, TLB,
// exceptions and interrupts. COP1 is in h64_fpu.cpp.
//
// Sources: NEC VR4300 user's manual (instruction set, chapter 6 "Exception
// processing", chapter 5 "Memory management"), n64brew wiki. Timing model:
// one PClock cycle per instruction (reference); Count = cycles / 2.
#include "h64_cpu_internal.h"

#include <string.h>

#include "../common/h64_log.h"

// ---------------------------------------------------------------------------
// Reset and COP0 basics
// ---------------------------------------------------------------------------

void h64_cpu_reset(H64Cpu *cpu)
{
    u64 cycles = cpu->cycles;
    void (*hook)(void *, int) = cpu->excHook;
    void *hookUser = cpu->excUser;
    memset(cpu, 0, sizeof(*cpu));
    cpu->cycles = cycles;
    cpu->excHook = hook;
    cpu->excUser = hookUser;
    cpu->cop0[CP0_RANDOM] = 31;
    cpu->cop0[CP0_PRID] = 0x00000B22;
    cpu->cop0[CP0_CONFIG] = 0x7006E463;
    cpu->cop0[CP0_STATUS] = 0x34000000;
    cpu->cop0[CP0_EPC] = 0xFFFFFFFFFFFFFFFFull;
    cpu->cop0[CP0_ERROREPC] = 0xFFFFFFFFFFFFFFFFull;
    cpu->cop0[CP0_BADVADDR] = 0xFFFFFFFFFFFFFFFFull;
    cpu->cop0[CP0_CONTEXT] = 0x007FFFF0;
    cpu->pc = 0xFFFFFFFFBFC00000ull;
    cpu->nextPc = cpu->pc + 4;
}

u32 h64_cpu_count(const H64Cpu *cpu)
{
    return (u32)(cpu->cycles >> 1) + cpu->countOffset;
}

void h64_cpu_reschedule_compare(H64System *sys)
{
    H64Cpu *cpu = &sys->cpu;
    u32 target = (u32)cpu->cop0[CP0_COMPARE] - cpu->countOffset;   // value of (cycles >> 1) to reach
    u64 base = cpu->cycles >> 1;
    u32 delta = target - (u32)base;
    u64 steps = delta ? (u64)delta : 0x100000000ull;
    h64_sched_set(&sys->sched, H64_EV_COMPARE, (base + steps) << 1);
}

void h64_cpu_set_ip(H64System *sys, int bit, int on)
{
    u64 mask = 1ull << (8 + bit);
    if (on) sys->cpu.cop0[CP0_CAUSE] |= mask;
    else sys->cpu.cop0[CP0_CAUSE] &= ~mask;
}

static int cpu_kernel_mode(const H64Cpu *cpu)
{
    u32 sr = (u32)cpu->cop0[CP0_STATUS];
    return (sr & (SR_EXL | SR_ERL)) || ((sr & SR_KSU) == 0);
}

static int cpu_supervisor_mode(const H64Cpu *cpu)
{
    u32 sr = (u32)cpu->cop0[CP0_STATUS];
    return !(sr & (SR_EXL | SR_ERL)) && ((sr & SR_KSU) == 0x08);
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

void h64_cpu_exception(H64Cpu *cpu, int code, u32 vectorOffset)
{
    u64 *cause = &cpu->cop0[CP0_CAUSE];
    u32 sr = (u32)cpu->cop0[CP0_STATUS];
    u64 base;
    if (!(sr & SR_EXL))
    {
        if (cpu->curInDelaySlot)
        {
            cpu->cop0[CP0_EPC] = cpu->curPc - 4;
            *cause |= 0x80000000ull;
        }
        else
        {
            cpu->cop0[CP0_EPC] = cpu->curPc;
            *cause &= ~0x80000000ull;
        }
    }
    else
        vectorOffset = 0x180;   // nested: TLB refills go to the general vector
    *cause = (*cause & ~0x7Cull) | ((u64)code << 2);
    if (code != EXC_CPU)
        *cause &= ~0x30000000ull;   // CE: only set for coprocessor-unusable (and COP2 reserved, by the caller)
    cpu->cop0[CP0_STATUS] |= SR_EXL;
    base = (sr & SR_BEV) ? 0xFFFFFFFFBFC00200ull : 0xFFFFFFFF80000000ull;
    cpu->pc = base + vectorOffset;
    cpu->nextPc = cpu->pc + 4;
    cpu->branchPending = 0;
    cpu->exceptionRaised = 1;
    if (cpu->excHook)
        cpu->excHook(cpu->excUser, code);
}

void h64_cpu_exception_cop(H64Cpu *cpu, int copNumber)
{
    cpu->cop0[CP0_CAUSE] = (cpu->cop0[CP0_CAUSE] & ~0x30000000ull) | ((u64)(copNumber & 3) << 28);
    h64_cpu_exception(cpu, EXC_CPU, 0x180);
}

static void address_error(H64Cpu *cpu, u64 vaddr, int write)
{
    // Address errors also load Context, XContext and EntryHi from the address (n64-systemtest).
    cpu->cop0[CP0_BADVADDR] = vaddr;
    cpu->cop0[CP0_CONTEXT] = (cpu->cop0[CP0_CONTEXT] & 0xFFFFFFFFFF800000ull) | ((vaddr >> 9) & 0x7FFFF0ull);
    cpu->cop0[CP0_XCONTEXT] = (cpu->cop0[CP0_XCONTEXT] & 0xFFFFFFFE00000000ull) |
                              (((vaddr >> 62) & 3) << 31) | (((vaddr >> 13) & 0x7FFFFFFull) << 4);
    cpu->cop0[CP0_ENTRYHI] = (vaddr & 0xC00000FFFFFFE000ull) | (cpu->cop0[CP0_ENTRYHI] & 0xFF);
    h64_cpu_exception(cpu, write ? EXC_ADES : EXC_ADEL, 0x180);
}

// ---------------------------------------------------------------------------
// Address translation
// ---------------------------------------------------------------------------

enum { ACC_READ = 0, ACC_WRITE = 1, ACC_FETCH = 2 };

// Fills BadVAddr, Context, XContext and EntryHi for a TLB exception.
static void tlb_exception(H64Cpu *cpu, u64 vaddr, int code, u32 vectorOffset)
{
    u64 vpn2 = (vaddr >> 13) & 0x7FFFFFFull;
    cpu->cop0[CP0_BADVADDR] = vaddr;
    cpu->cop0[CP0_CONTEXT] = (cpu->cop0[CP0_CONTEXT] & 0xFFFFFFFFFF800000ull) | ((vaddr >> 9) & 0x7FFFF0ull);
    cpu->cop0[CP0_XCONTEXT] = (cpu->cop0[CP0_XCONTEXT] & 0xFFFFFFFE00000000ull) |
                              (((vaddr >> 62) & 3) << 31) | (((vaddr >> 13) & 0x7FFFFFFull) << 4);
    cpu->cop0[CP0_ENTRYHI] = (vaddr & 0xC00000FFFFFFE000ull) | (cpu->cop0[CP0_ENTRYHI] & 0xFF);
    (void)vpn2;
    h64_cpu_exception(cpu, code, vectorOffset);
}

// TLB lookup. Returns 0 and the physical address, or raises the exception.
static int tlb_translate(H64Cpu *cpu, u64 vaddr, int access, int addr64, u32 *paddr)
{
    int i;
    u8 asid = (u8)cpu->cop0[CP0_ENTRYHI];
    for (i = 0; i < 32; i++)
    {
        const H64TlbEntry *e = &cpu->tlb[i];
        u64 maskFull = ((u64)e->pageMask | 0x1FFF);   // VPN2 bits below the page pair
        u64 vpnMask = 0xC00000FFFFFFE000ull & ~maskFull;
        int global = (e->entryLo0 & e->entryLo1 & 1);
        if ((vaddr & vpnMask) != (e->entryHi & vpnMask))
            continue;
        if (!global && (u8)e->entryHi != asid)
            continue;
        {
            u64 offsetMask = maskFull >> 1;            // bytes within one page
            u32 lo = (vaddr & (offsetMask + 1)) ? e->entryLo1 : e->entryLo0;
            int valid = (lo >> 1) & 1, dirty = (lo >> 2) & 1;
            if (!valid)
            {
                tlb_exception(cpu, vaddr, access == ACC_WRITE ? EXC_TLBS : EXC_TLBL, 0x180);
                return -1;
            }
            if (access == ACC_WRITE && !dirty)
            {
                tlb_exception(cpu, vaddr, EXC_MOD, 0x180);
                return -1;
            }
            *paddr = (u32)((((u64)(lo >> 6) & 0xFFFFF) << 12) & ~offsetMask) | (u32)(vaddr & offsetMask);
            return 0;
        }
    }
    tlb_exception(cpu, vaddr, access == ACC_WRITE ? EXC_TLBS : EXC_TLBL, addr64 ? 0x080 : 0x000);
    return -1;
}

static int translate(H64Cpu *cpu, u64 vaddr, int access, u32 *paddr)
{
    u32 sr = (u32)cpu->cop0[CP0_STATUS];
    int kernel = cpu_kernel_mode(cpu), super = cpu_supervisor_mode(cpu);
    if (vaddr == SEXT32(vaddr))
    {
        u32 a = (u32)vaddr;
        // The refill vector is XTLB (0x080) when the segment uses 64-bit addressing.
        if (a < 0x80000000u)
            return tlb_translate(cpu, vaddr, access, (sr & SR_UX) != 0, paddr);
        if (!kernel && !(super && a >= 0xC0000000u && a < 0xE0000000u))
        {
            address_error(cpu, vaddr, access == ACC_WRITE);
            return -1;
        }
        if (a < 0xA0000000u) { *paddr = a - 0x80000000u; return 0; }
        if (a < 0xC0000000u) { *paddr = a - 0xA0000000u; return 0; }
        return tlb_translate(cpu, vaddr, access, a < 0xE0000000u ? (sr & SR_SX) != 0 : (sr & SR_KX) != 0, paddr);
    }
    // 64-bit addresses: only valid with the matching extended-addressing bit.
    {
        u32 region = (u32)(vaddr >> 62);
        if (region == 0 && (sr & SR_UX) && (vaddr >> 40) == 0)
            return tlb_translate(cpu, vaddr, access, 1, paddr);
        if (region == 1 && (sr & SR_SX) && (kernel || super) && ((vaddr >> 40) & 0x3FFFFF) == 0)
            return tlb_translate(cpu, vaddr, access, 1, paddr);
        if (region == 2 && (sr & SR_KX) && kernel && ((vaddr >> 32) & 0x07FFFFFF) == 0)
        {
            *paddr = (u32)vaddr;   // xkphys: unmapped, cache attribute in bits 59..61
            return 0;
        }
        // xkseg: 0xC000000000000000-0xC00000FF7FFFFFFF (above that is ckseg, 32-bit compatible).
        if (region == 3 && (sr & SR_KX) && kernel && ((vaddr >> 40) & 0x3FFFFF) == 0 &&
            (vaddr & 0xFFFFFFFFFFull) < 0xFF80000000ull)
            return tlb_translate(cpu, vaddr, access, 1, paddr);
    }
    address_error(cpu, vaddr, access == ACC_WRITE);
    return -1;
}

int h64_cpu_translate_debug(H64Cpu *cpu, u64 vaddr, u32 *paddr)
{
    H64Cpu copy = *cpu;
    copy.exceptionRaised = 0;
    if (translate(&copy, vaddr, ACC_READ, paddr))
        return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Memory access with alignment checks
// ---------------------------------------------------------------------------

#define CHECK_ALIGN(cpu, vaddr, size, write) \
    if ((vaddr) & ((size) - 1)) { address_error((cpu), (vaddr), (write)); return -1; }

int h64_cpu_read8(H64System *sys, u64 vaddr, u8 *v)
{
    u32 p;
    if (translate(&sys->cpu, vaddr, ACC_READ, &p)) return -1;
    return h64_bus_read8(sys, p, v);
}
int h64_cpu_read16(H64System *sys, u64 vaddr, u16 *v)
{
    u32 p;
    CHECK_ALIGN(&sys->cpu, vaddr, 2, 0);
    if (translate(&sys->cpu, vaddr, ACC_READ, &p)) return -1;
    return h64_bus_read16(sys, p, v);
}
int h64_cpu_read32(H64System *sys, u64 vaddr, u32 *v)
{
    u32 p;
    CHECK_ALIGN(&sys->cpu, vaddr, 4, 0);
    if (translate(&sys->cpu, vaddr, ACC_READ, &p)) return -1;
    return h64_bus_read32(sys, p, v);
}
int h64_cpu_read64(H64System *sys, u64 vaddr, u64 *v)
{
    u32 p;
    CHECK_ALIGN(&sys->cpu, vaddr, 8, 0);
    if (translate(&sys->cpu, vaddr, ACC_READ, &p)) return -1;
    return h64_bus_read64(sys, p, v);
}
int h64_cpu_write8(H64System *sys, u64 vaddr, u32 v)
{
    u32 p;
    if (translate(&sys->cpu, vaddr, ACC_WRITE, &p)) return -1;
    return h64_bus_write8(sys, p, v);
}
int h64_cpu_write16(H64System *sys, u64 vaddr, u32 v)
{
    u32 p;
    CHECK_ALIGN(&sys->cpu, vaddr, 2, 1);
    if (translate(&sys->cpu, vaddr, ACC_WRITE, &p)) return -1;
    return h64_bus_write16(sys, p, v);
}
int h64_cpu_write32(H64System *sys, u64 vaddr, u32 v)
{
    u32 p;
    CHECK_ALIGN(&sys->cpu, vaddr, 4, 1);
    if (translate(&sys->cpu, vaddr, ACC_WRITE, &p)) return -1;
    return h64_bus_write32(sys, p, v, 0xFFFFFFFFu);
}
int h64_cpu_write64(H64System *sys, u64 vaddr, u64 v)
{
    u32 p;
    CHECK_ALIGN(&sys->cpu, vaddr, 8, 1);
    if (translate(&sys->cpu, vaddr, ACC_WRITE, &p)) return -1;
    return h64_bus_write64(sys, p, v);
}

// Unaligned-access helpers (LWL/LWR/SWL/SWR/LDL/LDR/SDL/SDR) work on the
// aligned word/doubleword containing the address.
static int read_aligned32(H64System *sys, u64 vaddr, u32 *v)
{
    u32 p;
    // Translate the raw address so that BadVAddr keeps its low bits.
    if (translate(&sys->cpu, vaddr, ACC_READ, &p)) return -1;
    return h64_bus_read32(sys, p & ~3u, v);
}
static int read_aligned64(H64System *sys, u64 vaddr, u64 *v)
{
    u32 p;
    if (translate(&sys->cpu, vaddr, ACC_READ, &p)) return -1;
    return h64_bus_read64(sys, p & ~7u, v);
}
static int write_aligned32(H64System *sys, u64 vaddr, u32 v, u32 mask)
{
    u32 p;
    if (translate(&sys->cpu, vaddr, ACC_WRITE, &p)) return -1;
    return h64_bus_write32(sys, p & ~3u, v, mask);
}
static int write_aligned64(H64System *sys, u64 vaddr, u64 v, u64 mask)
{
    u32 p;
    if (translate(&sys->cpu, vaddr, ACC_WRITE, &p)) return -1;
    p &= ~7u;
    if (h64_bus_write32(sys, p, (u32)(v >> 32), (u32)(mask >> 32))) return -1;
    return h64_bus_write32(sys, p + 4, (u32)v, (u32)mask);
}

// ---------------------------------------------------------------------------
// COP0 register access
// ---------------------------------------------------------------------------

static u32 cpu_random(H64Cpu *cpu)
{
    // Random decrements once per instruction from 31 down to Wired, then
    // wraps to 31. Kept in cop0[RANDOM] as the value at instruction 0 offset.
    u32 wired = (u32)cpu->cop0[CP0_WIRED] & 0x3F;
    u32 n = (u32)(cpu->instructions - cpu->cop0[7]);   // cop0[7] (unused) holds the reset point
    if (wired > 31)
        return (u32)(63 - (n & 63));
    return 31 - (n % (32 - wired));
}

static u64 cop0_read(H64System *sys, int r)
{
    H64Cpu *cpu = &sys->cpu;
    switch (r)
    {
    case CP0_RANDOM: return cpu_random(cpu);
    case CP0_COUNT: return h64_cpu_count(cpu);
    case 7: case 21: case 22: case 23: case 24: case 25: case 31: return cpu->cop0Latch;
    }
    return cpu->cop0[r];
}

static void cop0_write(H64System *sys, int r, u64 v)
{
    H64Cpu *cpu = &sys->cpu;
    cpu->cop0Latch = v;
    switch (r)
    {
    case CP0_INDEX: cpu->cop0[r] = v & 0x8000003Fu; break;
    case CP0_RANDOM: break;
    case CP0_ENTRYLO0: case CP0_ENTRYLO1: cpu->cop0[r] = v & 0x3FFFFFFFu; break;
    case CP0_CONTEXT: cpu->cop0[r] = (v & 0xFFFFFFFFFF800000ull) | (cpu->cop0[r] & 0x7FFFF0ull); break;
    case CP0_PAGEMASK: cpu->cop0[r] = v & 0x01FFE000u; break;
    case CP0_WIRED: cpu->cop0[r] = v & 0x3F; cpu->cop0[7] = cpu->instructions; break;
    case CP0_BADVADDR: break;
    case CP0_COUNT:
        cpu->countOffset = (u32)v - (u32)(cpu->cycles >> 1);
        h64_cpu_reschedule_compare(sys);
        break;
    case CP0_ENTRYHI: cpu->cop0[r] = v & 0xC00000FFFFFFE0FFull; break;
    case CP0_COMPARE:
        cpu->cop0[r] = (u32)v;
        h64_cpu_set_ip(sys, 7, 0);
        h64_cpu_reschedule_compare(sys);
        break;
    case CP0_STATUS: cpu->cop0[r] = (u32)v & 0xFF57FFFFu; break;
    case CP0_CAUSE: cpu->cop0[r] = (cpu->cop0[r] & ~0x300ull) | (v & 0x300); break;
    case CP0_PRID: break;
    case CP0_CONFIG: cpu->cop0[r] = (cpu->cop0[r] & ~0x0F00800Full) | (v & 0x0F00800F); break;
    case CP0_LLADDR: cpu->cop0[r] = (u32)v; break;
    case CP0_WATCHLO: cpu->cop0[r] = (u32)v & 0xFFFFFFFBu; break;
    case CP0_WATCHHI: cpu->cop0[r] = v & 0xF; break;
    case CP0_XCONTEXT: cpu->cop0[r] = (v & 0xFFFFFFFE00000000ull) | (cpu->cop0[r] & 0x1FFFFFFFFull); break;
    case CP0_PARITYERROR: cpu->cop0[r] = v & 0xFF; break;
    case CP0_CACHEERROR: break;
    case CP0_TAGLO: cpu->cop0[r] = v & 0x0FFFFFC0u; break;
    case CP0_TAGHI: break;
    case 7: case 21: case 22: case 23: case 24: case 25: case 31: break;
    default: cpu->cop0[r] = v; break;
    }
}

static void tlb_read(H64Cpu *cpu)
{
    const H64TlbEntry *e = &cpu->tlb[cpu->cop0[CP0_INDEX] & 31];
    u32 g = e->entryLo0 & e->entryLo1 & 1;
    cpu->cop0[CP0_PAGEMASK] = e->pageMask;
    cpu->cop0[CP0_ENTRYHI] = e->entryHi & ~(u64)e->pageMask;
    cpu->cop0[CP0_ENTRYLO0] = (e->entryLo0 & ~1u) | g;
    cpu->cop0[CP0_ENTRYLO1] = (e->entryLo1 & ~1u) | g;
}

static void tlb_write(H64Cpu *cpu, int index)
{
    H64TlbEntry *e = &cpu->tlb[index & 31];
    // The TLB keeps one bit per PageMask pair (the upper one) and a 20-bit PFN.
    e->pageMask = (u32)cpu->cop0[CP0_PAGEMASK] & 0x01554000u;
    e->pageMask |= e->pageMask >> 1;
    e->entryHi = cpu->cop0[CP0_ENTRYHI] & ~(u64)e->pageMask;
    e->entryLo0 = (u32)cpu->cop0[CP0_ENTRYLO0] & 0x03FFFFFFu;
    e->entryLo1 = (u32)cpu->cop0[CP0_ENTRYLO1] & 0x03FFFFFFu;
    // The G bit of the entry is the AND of both EntryLo G bits.
    if (!((cpu->cop0[CP0_ENTRYLO0] & cpu->cop0[CP0_ENTRYLO1]) & 1))
    {
        e->entryLo0 &= ~1u;
        e->entryLo1 &= ~1u;
    }
}

static void tlb_probe(H64Cpu *cpu)
{
    int i;
    u64 hi = cpu->cop0[CP0_ENTRYHI];
    cpu->cop0[CP0_INDEX] |= 0x80000000u;
    for (i = 0; i < 32; i++)
    {
        const H64TlbEntry *e = &cpu->tlb[i];
        u64 vpnMask = 0xC00000FFFFFFE000ull & ~((u64)e->pageMask | 0x1FFF);
        if ((hi & vpnMask) != (e->entryHi & vpnMask)) continue;
        if (!(e->entryLo0 & e->entryLo1 & 1) && (u8)e->entryHi != (u8)hi) continue;
        cpu->cop0[CP0_INDEX] = (u32)i;
        return;
    }
}

static void do_cop0(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    if (!cpu_kernel_mode(cpu) && !(cpu->cop0[CP0_STATUS] & SR_CU0))
    {
        h64_cpu_exception_cop(cpu, 0);
        return;
    }
    if (op & 0x02000000u)   // CO: bits 21..24 are ignored
    {
        switch (FUNCT(op))
        {
        case 0x01: tlb_read(cpu); return;                                          // TLBR
        case 0x02: tlb_write(cpu, (int)(cpu->cop0[CP0_INDEX] & 31)); return;       // TLBWI
        case 0x06: tlb_write(cpu, (int)cpu_random(cpu)); return;                   // TLBWR
        case 0x08: tlb_probe(cpu); return;                                         // TLBP
        case 0x18:                                                                 // ERET
            if (cpu->cop0[CP0_STATUS] & SR_ERL)
            {
                cpu->pc = cpu->cop0[CP0_ERROREPC];
                cpu->cop0[CP0_STATUS] &= ~(u64)SR_ERL;
            }
            else
            {
                cpu->pc = cpu->cop0[CP0_EPC];
                cpu->cop0[CP0_STATUS] &= ~(u64)SR_EXL;
            }
            cpu->nextPc = cpu->pc + 4;
            cpu->branchPending = 0;
            cpu->llbit = 0;
            return;
        }
        if (sys->options.emux)
        {
            // EMUX emulator extensions (n64-systemtest src/emux.rs): rd in
            // bits 20..24, rt in bits 15..19, code in bits 6..14.
            int erd = (op >> 20) & 31, ert = (op >> 15) & 31, code = (op >> 6) & 0x1FF;
            switch (FUNCT(op))
            {
            case 0x20:   // XDETECT: code 1 = mask of supported functions 0x20..0x3F
                cpu->gpr[erd] = code == 1 ? ((1u << 0) | (1u << 5) | (1u << 12)) : 0;
                cpu->gpr[0] = 0;
                return;
            case 0x25:   // XLOG: text at gpr[rd], length in gpr[rt]
            {
                u64 p = cpu->gpr[erd];
                u32 len = (u32)cpu->gpr[ert], i;
                u8 buf[256];
                while (len > 0)
                {
                    u32 n = len > sizeof(buf) ? (u32)sizeof(buf) : len;
                    for (i = 0; i < n; i++)
                    {
                        u32 pa;
                        u8 b = '?';
                        if (h64_cpu_translate_debug(cpu, p + i, &pa)) h64_bus_read8(sys, pa, &b);
                        buf[i] = b;
                    }
                    h64_debug_text(sys, buf, n);
                    p += n;
                    len -= n;
                }
                return;
            }
            case 0x2C:   // XIOCTL: 1 = exit, 2 = fast (no effect: timing is already untimed)
                if (code == 1) { sys->exitRequested = 1; sys->stop = 1; }
                return;
            }
        }
        // Other CO functions do nothing on the VR4300 (inferred: n64-systemtest
        // runs its EMUX detection on real hardware, which only works if the
        // unknown COP0 function does not trap).
        return;
    }
    switch (RS(op))
    {
    case 0x00: cpu->gpr[RT(op)] = SEXT32(cop0_read(sys, RD(op))); return;          // MFC0
    case 0x01: cpu->gpr[RT(op)] = cop0_read(sys, RD(op)); return;                  // DMFC0
    case 0x04: cop0_write(sys, RD(op), SEXT32(cpu->gpr[RT(op)])); return;          // MTC0
    case 0x05: cop0_write(sys, RD(op), cpu->gpr[RT(op)]); return;                  // DMTC0
    }

    h64_cpu_exception(cpu, EXC_RI, 0x180);
}

// ---------------------------------------------------------------------------
// Branch helpers
// ---------------------------------------------------------------------------

static void branch(H64Cpu *cpu, int taken, u32 op)
{
    if (taken)
        cpu->nextPc = cpu->curPc + 4 + (IMM16(op) << 2);
    cpu->branchPending = 1;
}

static void branch_likely(H64Cpu *cpu, int taken, u32 op)
{
    if (taken)
    {
        cpu->nextPc = cpu->curPc + 4 + (IMM16(op) << 2);
        cpu->branchPending = 1;
    }
    else
    {
        // The delay slot is skipped.
        cpu->pc = cpu->nextPc;
        cpu->nextPc = cpu->pc + 4;
    }
}

static void jump(H64Cpu *cpu, u64 target)
{
    cpu->nextPc = target;
    cpu->branchPending = 1;
}

// 64x64 -> 128-bit multiply, portable (no __int128 on MSVC).
static void mul64(u64 a, u64 b, u64 *hi, u64 *lo)
{
    u64 a0 = (u32)a, a1 = a >> 32, b0 = (u32)b, b1 = b >> 32;
    u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    u64 mid = (p00 >> 32) + (u32)p01 + (u32)p10;
    *lo = (mid << 32) | (u32)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static int add32_overflows(u32 a, u32 b, u32 r) { return ((~(a ^ b) & (a ^ r)) >> 31) != 0; }
static int sub32_overflows(u32 a, u32 b, u32 r) { return (((a ^ b) & (a ^ r)) >> 31) != 0; }
static int add64_overflows(u64 a, u64 b, u64 r) { return ((~(a ^ b) & (a ^ r)) >> 63) != 0; }
static int sub64_overflows(u64 a, u64 b, u64 r) { return (((a ^ b) & (a ^ r)) >> 63) != 0; }

// ---------------------------------------------------------------------------
// SPECIAL and REGIMM
// ---------------------------------------------------------------------------

static void do_special(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    u64 *g = cpu->gpr;
    u64 rs = g[RS(op)], rt = g[RT(op)];
    int rd = RD(op), sa = SA(op);
    switch (FUNCT(op))
    {
    case 0x00: g[rd] = SEXT32((u32)rt << sa); return;                              // SLL
    case 0x02: g[rd] = SEXT32((u32)rt >> sa); return;                              // SRL
    // SRA/SRAV shift the whole 64-bit register, then sign-extend bit 31 of the result (VR4300).
    case 0x03: g[rd] = SEXT32((u32)((s64)rt >> sa)); return;                       // SRA
    case 0x04: g[rd] = SEXT32((u32)rt << (rs & 31)); return;                       // SLLV
    case 0x06: g[rd] = SEXT32((u32)rt >> (rs & 31)); return;                       // SRLV
    case 0x07: g[rd] = SEXT32((u32)((s64)rt >> (rs & 31))); return;                // SRAV
    case 0x08: jump(cpu, rs); return;                                              // JR
    case 0x09: g[rd] = cpu->nextPc; jump(cpu, rs); return;                         // JALR (link: see JAL)
    case 0x0C: h64_cpu_exception(cpu, EXC_SYS, 0x180); return;                     // SYSCALL
    case 0x0D: h64_cpu_exception(cpu, EXC_BP, 0x180); return;                      // BREAK
    case 0x0F: return;                                                             // SYNC
    case 0x10: g[rd] = cpu->hi; return;                                            // MFHI
    case 0x11: cpu->hi = rs; return;                                               // MTHI
    case 0x12: g[rd] = cpu->lo; return;                                            // MFLO
    case 0x13: cpu->lo = rs; return;                                               // MTLO
    case 0x14: g[rd] = rt << (rs & 63); return;                                    // DSLLV
    case 0x16: g[rd] = rt >> (rs & 63); return;                                    // DSRLV
    case 0x17: g[rd] = (u64)((s64)rt >> (rs & 63)); return;                        // DSRAV
    case 0x18:                                                                     // MULT
    {
        s64 p = (s64)(s32)(u32)rs * (s64)(s32)(u32)rt;
        cpu->lo = SEXT32((u32)p);
        cpu->hi = SEXT32((u32)((u64)p >> 32));
        return;
    }
    case 0x19:                                                                     // MULTU
    {
        u64 p = (u64)(u32)rs * (u64)(u32)rt;
        cpu->lo = SEXT32((u32)p);
        cpu->hi = SEXT32((u32)(p >> 32));
        return;
    }
    case 0x1A:                                                                     // DIV
    {
        s32 a = (s32)(u32)rs, b = (s32)(u32)rt;
        if (b == 0) { cpu->lo = a < 0 ? 1 : 0xFFFFFFFFFFFFFFFFull; cpu->hi = SEXT32(a); }
        else if (a == (s32)0x80000000 && b == -1) { cpu->lo = SEXT32(a); cpu->hi = 0; }
        else { cpu->lo = SEXT32(a / b); cpu->hi = SEXT32(a % b); }
        return;
    }
    case 0x1B:                                                                     // DIVU
    {
        u32 a = (u32)rs, b = (u32)rt;
        if (b == 0) { cpu->lo = 0xFFFFFFFFFFFFFFFFull; cpu->hi = SEXT32(a); }
        else { cpu->lo = SEXT32(a / b); cpu->hi = SEXT32(a % b); }
        return;
    }
    case 0x1C:                                                                     // DMULT
    {
        u64 hi, lo;
        mul64(rs, rt, &hi, &lo);
        if ((s64)rs < 0) hi -= rt;
        if ((s64)rt < 0) hi -= rs;
        cpu->hi = hi; cpu->lo = lo;
        return;
    }
    case 0x1D: mul64(rs, rt, &cpu->hi, &cpu->lo); return;                          // DMULTU
    case 0x1E:                                                                     // DDIV
    {
        s64 a = (s64)rs, b = (s64)rt;
        if (b == 0) { cpu->lo = a < 0 ? 1 : 0xFFFFFFFFFFFFFFFFull; cpu->hi = (u64)a; }
        else if (rs == 0x8000000000000000ull && b == -1) { cpu->lo = rs; cpu->hi = 0; }
        else { cpu->lo = (u64)(a / b); cpu->hi = (u64)(a % b); }
        return;
    }
    case 0x1F:                                                                     // DDIVU
        if (rt == 0) { cpu->lo = 0xFFFFFFFFFFFFFFFFull; cpu->hi = rs; }
        else { cpu->lo = rs / rt; cpu->hi = rs % rt; }
        return;
    case 0x20:                                                                     // ADD
    {
        u32 r = (u32)rs + (u32)rt;
        if (add32_overflows((u32)rs, (u32)rt, r)) { h64_cpu_exception(cpu, EXC_OV, 0x180); return; }
        g[rd] = SEXT32(r);
        return;
    }
    case 0x21: g[rd] = SEXT32((u32)rs + (u32)rt); return;                          // ADDU
    case 0x22:                                                                     // SUB
    {
        u32 r = (u32)rs - (u32)rt;
        if (sub32_overflows((u32)rs, (u32)rt, r)) { h64_cpu_exception(cpu, EXC_OV, 0x180); return; }
        g[rd] = SEXT32(r);
        return;
    }
    case 0x23: g[rd] = SEXT32((u32)rs - (u32)rt); return;                          // SUBU
    case 0x24: g[rd] = rs & rt; return;                                            // AND
    case 0x25: g[rd] = rs | rt; return;                                            // OR
    case 0x26: g[rd] = rs ^ rt; return;                                            // XOR
    case 0x27: g[rd] = ~(rs | rt); return;                                         // NOR
    case 0x2A: g[rd] = (s64)rs < (s64)rt ? 1 : 0; return;                          // SLT
    case 0x2B: g[rd] = rs < rt ? 1 : 0; return;                                    // SLTU
    case 0x2C:                                                                     // DADD
    {
        u64 r = rs + rt;
        if (add64_overflows(rs, rt, r)) { h64_cpu_exception(cpu, EXC_OV, 0x180); return; }
        g[rd] = r;
        return;
    }
    case 0x2D: g[rd] = rs + rt; return;                                            // DADDU
    case 0x2E:                                                                     // DSUB
    {
        u64 r = rs - rt;
        if (sub64_overflows(rs, rt, r)) { h64_cpu_exception(cpu, EXC_OV, 0x180); return; }
        g[rd] = r;
        return;
    }
    case 0x2F: g[rd] = rs - rt; return;                                            // DSUBU
    case 0x30: if ((s64)rs >= (s64)rt) h64_cpu_exception(cpu, EXC_TR, 0x180); return;   // TGE
    case 0x31: if (rs >= rt) h64_cpu_exception(cpu, EXC_TR, 0x180); return;             // TGEU
    case 0x32: if ((s64)rs < (s64)rt) h64_cpu_exception(cpu, EXC_TR, 0x180); return;    // TLT
    case 0x33: if (rs < rt) h64_cpu_exception(cpu, EXC_TR, 0x180); return;              // TLTU
    case 0x34: if (rs == rt) h64_cpu_exception(cpu, EXC_TR, 0x180); return;             // TEQ
    case 0x36: if (rs != rt) h64_cpu_exception(cpu, EXC_TR, 0x180); return;             // TNE
    case 0x38: g[rd] = rt << sa; return;                                           // DSLL
    case 0x3A: g[rd] = rt >> sa; return;                                           // DSRL
    case 0x3B: g[rd] = (u64)((s64)rt >> sa); return;                               // DSRA
    case 0x3C: g[rd] = rt << (sa + 32); return;                                    // DSLL32
    case 0x3E: g[rd] = rt >> (sa + 32); return;                                    // DSRL32
    case 0x3F: g[rd] = (u64)((s64)rt >> (sa + 32)); return;                        // DSRA32
    }
    h64_cpu_exception(cpu, EXC_RI, 0x180);
}

static void do_regimm(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    s64 rs = (s64)cpu->gpr[RS(op)];
    u64 imm = IMM16(op);
    switch (RT(op))
    {
    case 0x00: branch(cpu, rs < 0, op); return;                                    // BLTZ
    case 0x01: branch(cpu, rs >= 0, op); return;                                   // BGEZ
    case 0x02: branch_likely(cpu, rs < 0, op); return;                             // BLTZL
    case 0x03: branch_likely(cpu, rs >= 0, op); return;                            // BGEZL
    case 0x08: if (rs >= (s64)imm) h64_cpu_exception(cpu, EXC_TR, 0x180); return;  // TGEI
    case 0x09: if ((u64)rs >= imm) h64_cpu_exception(cpu, EXC_TR, 0x180); return;  // TGEIU
    case 0x0A: if (rs < (s64)imm) h64_cpu_exception(cpu, EXC_TR, 0x180); return;   // TLTI
    case 0x0B: if ((u64)rs < imm) h64_cpu_exception(cpu, EXC_TR, 0x180); return;   // TLTIU
    case 0x0C: if ((u64)rs == imm) h64_cpu_exception(cpu, EXC_TR, 0x180); return;  // TEQI
    case 0x0E: if ((u64)rs != imm) h64_cpu_exception(cpu, EXC_TR, 0x180); return;  // TNEI
    case 0x10: cpu->gpr[31] = cpu->nextPc; branch(cpu, rs < 0, op); return;        // BLTZAL
    case 0x11: cpu->gpr[31] = cpu->nextPc; branch(cpu, rs >= 0, op); return;       // BGEZAL
    case 0x12: cpu->gpr[31] = cpu->nextPc; branch_likely(cpu, rs < 0, op); return;      // BLTZALL
    case 0x13: cpu->gpr[31] = cpu->nextPc; branch_likely(cpu, rs >= 0, op); return;     // BGEZALL
    }
    h64_cpu_exception(cpu, EXC_RI, 0x180);
}

// ---------------------------------------------------------------------------
// Loads and stores
// ---------------------------------------------------------------------------

static void do_load_store(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    u64 *g = cpu->gpr;
    u64 addr = g[RS(op)] + IMM16(op);
    int rt = RT(op);
    switch (op >> 26)
    {
    case 0x20: { u8 v; if (!h64_cpu_read8(sys, addr, &v)) g[rt] = (u64)(s64)(s8)v; return; }     // LB
    case 0x24: { u8 v; if (!h64_cpu_read8(sys, addr, &v)) g[rt] = v; return; }                   // LBU
    case 0x21: { u16 v; if (!h64_cpu_read16(sys, addr, &v)) g[rt] = (u64)(s64)(s16)v; return; }  // LH
    case 0x25: { u16 v; if (!h64_cpu_read16(sys, addr, &v)) g[rt] = v; return; }                 // LHU
    case 0x23: { u32 v; if (!h64_cpu_read32(sys, addr, &v)) g[rt] = SEXT32(v); return; }         // LW
    case 0x27: { u32 v; if (!h64_cpu_read32(sys, addr, &v)) g[rt] = v; return; }                 // LWU
    case 0x37: { u64 v; if (!h64_cpu_read64(sys, addr, &v)) g[rt] = v; return; }                 // LD
    case 0x28: h64_cpu_write8(sys, addr, (u32)g[rt]); return;                                    // SB
    case 0x29: h64_cpu_write16(sys, addr, (u32)g[rt]); return;                                   // SH
    case 0x2B: h64_cpu_write32(sys, addr, (u32)g[rt]); return;                                   // SW
    case 0x3F: h64_cpu_write64(sys, addr, g[rt]); return;                                        // SD
    case 0x22:                                                                                    // LWL
    {
        u32 w, sh = 8 * (u32)(addr & 3);
        if (read_aligned32(sys, addr, &w)) return;
        g[rt] = SEXT32(((u32)g[rt] & ~(0xFFFFFFFFu << sh)) | (w << sh));
        return;
    }
    case 0x26:                                                                                    // LWR
    {
        u32 w, sh = 8 * (3 - (u32)(addr & 3));
        if (read_aligned32(sys, addr, &w)) return;
        // A partial LWR keeps bits 32..63; loading the whole word sign-extends it.
        if (sh == 0) g[rt] = SEXT32(w);
        else g[rt] = (g[rt] & 0xFFFFFFFF00000000ull) | (((u32)g[rt] & ~(0xFFFFFFFFu >> sh)) | (w >> sh));
        return;
    }
    case 0x1A:                                                                                    // LDL
    {
        u64 d;
        u32 sh = 8 * (u32)(addr & 7);
        if (read_aligned64(sys, addr, &d)) return;
        g[rt] = sh ? ((g[rt] & ~(0xFFFFFFFFFFFFFFFFull << sh)) | (d << sh)) : d;
        return;
    }
    case 0x1B:                                                                                    // LDR
    {
        u64 d;
        u32 sh = 8 * (7 - (u32)(addr & 7));
        if (read_aligned64(sys, addr, &d)) return;
        g[rt] = sh ? ((g[rt] & ~(0xFFFFFFFFFFFFFFFFull >> sh)) | (d >> sh)) : d;
        return;
    }
    case 0x2A:                                                                                    // SWL
    {
        u32 sh = 8 * (u32)(addr & 3);
        write_aligned32(sys, addr, (u32)g[rt] >> sh, 0xFFFFFFFFu >> sh);
        return;
    }
    case 0x2E:                                                                                    // SWR
    {
        u32 sh = 8 * (3 - (u32)(addr & 3));
        write_aligned32(sys, addr, (u32)g[rt] << sh, 0xFFFFFFFFu << sh);
        return;
    }
    case 0x2C:                                                                                    // SDL
    {
        u32 sh = 8 * (u32)(addr & 7);
        write_aligned64(sys, addr, g[rt] >> sh, 0xFFFFFFFFFFFFFFFFull >> sh);
        return;
    }
    case 0x2D:                                                                                    // SDR
    {
        u32 sh = 8 * (7 - (u32)(addr & 7));
        write_aligned64(sys, addr, g[rt] << sh, 0xFFFFFFFFFFFFFFFFull << sh);
        return;
    }
    case 0x30:                                                                                    // LL
    {
        u32 v, p;
        if (h64_cpu_read32(sys, addr, &v)) return;
        g[rt] = SEXT32(v);
        cpu->llbit = 1;
        if (!h64_cpu_translate_debug(cpu, addr, &p)) p = 0;
        cpu->cop0[CP0_LLADDR] = p >> 4;
        return;
    }
    case 0x34:                                                                                    // LLD
    {
        u64 v;
        u32 p;
        if (h64_cpu_read64(sys, addr, &v)) return;
        g[rt] = v;
        cpu->llbit = 1;
        if (!h64_cpu_translate_debug(cpu, addr, &p)) p = 0;
        cpu->cop0[CP0_LLADDR] = p >> 4;
        return;
    }
    case 0x38:                                                                                    // SC
        if (cpu->llbit)
        {
            if (h64_cpu_write32(sys, addr, (u32)g[rt])) return;
            g[rt] = 1;
        }
        else
        {
            u32 p;
            // Still checked for exceptions even when the store does not happen.
            if (addr & 3) { address_error(cpu, addr, 1); return; }
            if (translate(cpu, addr, ACC_WRITE, &p)) return;
            g[rt] = 0;
        }
        return;
    case 0x3C:                                                                                    // SCD
        if (cpu->llbit)
        {
            if (h64_cpu_write64(sys, addr, g[rt])) return;
            g[rt] = 1;
        }
        else
        {
            u32 p;
            if (addr & 7) { address_error(cpu, addr, 1); return; }
            if (translate(cpu, addr, ACC_WRITE, &p)) return;
            g[rt] = 0;
        }
        return;
    }
}

// FPU loads/stores (need CU1).
static void do_fpu_load_store(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    u64 addr = cpu->gpr[RS(op)] + IMM16(op);
    int ft = RT(op);
    if (!(cpu->cop0[CP0_STATUS] & SR_CU1)) { h64_cpu_exception_cop(cpu, 1); return; }
    switch (op >> 26)
    {
    case 0x31: { u32 v; if (!h64_cpu_read32(sys, addr, &v)) h64_fpr_set32(cpu, ft, v); return; }   // LWC1
    case 0x35: { u64 v; if (!h64_cpu_read64(sys, addr, &v)) h64_fpr_set64(cpu, ft, v); return; }   // LDC1
    case 0x39: h64_cpu_write32(sys, addr, h64_fpr_get32(cpu, ft)); return;                         // SWC1
    case 0x3D: h64_cpu_write64(sys, addr, h64_fpr_get64(cpu, ft)); return;                         // SDC1
    }
}

// ---------------------------------------------------------------------------
// Main decode
// ---------------------------------------------------------------------------

static void execute(H64System *sys, u32 op)
{
    H64Cpu *cpu = &sys->cpu;
    u64 *g = cpu->gpr;
    u64 rs = g[RS(op)], rt = g[RT(op)];
    int t = RT(op);
    switch (op >> 26)
    {
    case 0x00: do_special(sys, op); return;
    case 0x01: do_regimm(sys, op); return;
    case 0x02: jump(cpu, ((cpu->curPc + 4) & 0xFFFFFFFFF0000000ull) | ((u64)(op & 0x03FFFFFF) << 2)); return;   // J
    case 0x03:                                                                                                  // JAL
        // The link is the address after the delay slot in execution order: for a
        // JAL itself in a delay slot, the first jump's target + 4 (n64-systemtest).
        g[31] = cpu->nextPc;
        jump(cpu, ((cpu->curPc + 4) & 0xFFFFFFFFF0000000ull) | ((u64)(op & 0x03FFFFFF) << 2));
        return;
    case 0x04: branch(cpu, rs == rt, op); return;                                  // BEQ
    case 0x05: branch(cpu, rs != rt, op); return;                                  // BNE
    case 0x06: branch(cpu, (s64)rs <= 0, op); return;                              // BLEZ
    case 0x07: branch(cpu, (s64)rs > 0, op); return;                               // BGTZ
    case 0x08:                                                                     // ADDI
    {
        u32 r = (u32)rs + (u32)IMM16(op);
        if (add32_overflows((u32)rs, (u32)IMM16(op), r)) { h64_cpu_exception(cpu, EXC_OV, 0x180); return; }
        g[t] = SEXT32(r);
        return;
    }
    case 0x09: g[t] = SEXT32((u32)rs + (u32)IMM16(op)); return;                    // ADDIU
    case 0x0A: g[t] = (s64)rs < (s64)IMM16(op) ? 1 : 0; return;                    // SLTI
    case 0x0B: g[t] = rs < IMM16(op) ? 1 : 0; return;                              // SLTIU
    case 0x0C: g[t] = rs & IMMU16(op); return;                                     // ANDI
    case 0x0D: g[t] = rs | IMMU16(op); return;                                     // ORI
    case 0x0E: g[t] = rs ^ IMMU16(op); return;                                     // XORI
    case 0x0F: g[t] = SEXT32((u32)(op & 0xFFFF) << 16); return;                    // LUI
    case 0x10: do_cop0(sys, op); return;                                           // COP0
    case 0x11:                                                                     // COP1
        if (!(cpu->cop0[CP0_STATUS] & SR_CU1)) { h64_cpu_exception_cop(cpu, 1); return; }
        h64_cop1_execute(sys, op);
        return;
    case 0x12:                                                                     // COP2
        if (!(cpu->cop0[CP0_STATUS] & 0x40000000u)) { h64_cpu_exception_cop(cpu, 2); return; }
        switch (RS(op))
        {
        case 0x00: case 0x02: g[t] = SEXT32((u32)cpu->cop2Latch); return;          // MFC2, CFC2
        case 0x01: g[t] = cpu->cop2Latch; return;                                  // DMFC2
        case 0x04: case 0x05: case 0x06: cpu->cop2Latch = rt; return;              // MTC2, DMTC2, CTC2
        }
        // Other COP2 encodings: Reserved Instruction, with Cause.CE = 2.
        h64_cpu_exception(cpu, EXC_RI, 0x180);
        cpu->cop0[CP0_CAUSE] |= 2ull << 28;
        return;
    case 0x13: h64_cpu_exception(cpu, EXC_RI, 0x180); return;                      // COP3
    case 0x14: branch_likely(cpu, rs == rt, op); return;                           // BEQL
    case 0x15: branch_likely(cpu, rs != rt, op); return;                           // BNEL
    case 0x16: branch_likely(cpu, (s64)rs <= 0, op); return;                       // BLEZL
    case 0x17: branch_likely(cpu, (s64)rs > 0, op); return;                        // BGTZL
    case 0x18:                                                                     // DADDI
    {
        u64 r = rs + IMM16(op);
        if (add64_overflows(rs, IMM16(op), r)) { h64_cpu_exception(cpu, EXC_OV, 0x180); return; }
        g[t] = r;
        return;
    }
    case 0x19: g[t] = rs + IMM16(op); return;                                      // DADDIU
    case 0x2F: return;                                                             // CACHE (caches not emulated)
    case 0x31: case 0x35: case 0x39: case 0x3D: do_fpu_load_store(sys, op); return;
    case 0x32: case 0x36: case 0x3A: case 0x3E:                                    // LWC2/LDC2/SWC2/SDC2
        if (!(cpu->cop0[CP0_STATUS] & 0x40000000u)) { h64_cpu_exception_cop(cpu, 2); return; }
        h64_cpu_exception(cpu, EXC_RI, 0x180);
        return;
    case 0x1A: case 0x1B: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
    case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x30:
    case 0x34: case 0x37: case 0x38: case 0x3C: case 0x3F:
        do_load_store(sys, op);
        return;
    }
    h64_cpu_exception(cpu, EXC_RI, 0x180);
}

void h64_cpu_step(H64System *sys)
{
    H64Cpu *cpu = &sys->cpu;
    u32 op, paddr;
    u32 sr = (u32)cpu->cop0[CP0_STATUS];

    cpu->curPc = cpu->pc;
    cpu->curInDelaySlot = cpu->branchPending;
    cpu->exceptionRaised = 0;

    // Interrupts are taken before the instruction at pc runs.
    if ((sr & SR_IE) && !(sr & (SR_EXL | SR_ERL)) && (cpu->cop0[CP0_CAUSE] & sr & SR_IM))
    {
        h64_cpu_exception(cpu, EXC_INT, 0x180);
        cpu->cycles++;
        return;
    }

    if (cpu->pc & 3)
    {
        address_error(cpu, cpu->pc, 0);
        cpu->cycles++;
        return;
    }
    if (translate(cpu, cpu->pc, ACC_FETCH, &paddr) || h64_bus_read32(sys, paddr, &op))
    {
        cpu->cycles++;
        return;
    }

    cpu->branchPending = 0;
    cpu->pc = cpu->nextPc;
    cpu->nextPc += 4;
    execute(sys, op);
    cpu->gpr[0] = 0;
    cpu->cycles++;
    cpu->instructions++;
}
