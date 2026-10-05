// Harissa64 V2 - physical address map (n64brew "Memory map").
//
//   0x00000000-0x03EFFFFF  RDRAM (8 MB present, the rest reads as 0)
//   0x03F00000-0x03FFFFFF  RDRAM registers
//   0x04000000-0x0403FFFF  RSP DMEM/IMEM (8 KB, mirrored)
//   0x04040000-0x048FFFFF  RCP registers (SP, DP, MI, VI, AI, PI, RI, SI)
//   0x05000000-0x0FFFFFFF  cartridge domains 1/2 (64DD, SRAM, FlashRAM)
//   0x10000000-0x1FBFFFFF  cartridge ROM (ISViewer at 0x13FF0000)
//   0x1FC00000-0x1FC007BF  PIF boot ROM (not available: HLE boot)
//   0x1FC007C0-0x1FC007FF  PIF RAM
//
// RDRAM, RSP memory and PIF RAM take byte-precise accesses. Smaller-than-32-
// bit writes to RCP registers reach the register as a 32-bit write of the
// data shifted into its byte lane (the RCP bus is 32 bits wide).
#include "../system/h64_system.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"

#define ISV_BASE 0x13FF0000u
#define ISV_END  0x13FF1000u

// Reads from a cartridge domain with nothing behind the address return the
// last value on the PI bus: the low 16 bits of the address, twice.
static u32 open_bus(u32 paddr)
{
    return ((paddr & 0xFFFFu) << 16) | (paddr & 0xFFFFu);
}

// Debug text from the guest (ISViewer, EMUX XLOG), split into lines.
void h64_debug_text(H64System *sys, const u8 *text, u32 len)
{
    u32 i;
    for (i = 0; i < len; i++)
    {
        char c = (char)text[i];
        if (c == '\n' || sys->isvLineLen >= (int)sizeof(sys->isvLine) - 1)
        {
            sys->isvLine[sys->isvLineLen] = 0;
            if (sys->isvSink) sys->isvSink(sys->isvUser, sys->isvLine);
            else H64_INFO("[isv] %s", sys->isvLine);
            sys->isvLineLen = 0;
            if (c == '\n') continue;
        }
        if (c != '\r')
            sys->isvLine[sys->isvLineLen++] = c;
    }
}

static void isv_flush(H64System *sys, u32 len)
{
    if (len > sizeof(sys->isvBuffer))
        len = sizeof(sys->isvBuffer);
    h64_debug_text(sys, sys->isvBuffer, len);
}

int h64_bus_read32(H64System *sys, u32 paddr, u32 *value)
{
    paddr &= ~3u;
    if (paddr < 0x03F00000u)
    {
        *value = paddr < H64_RDRAM_SIZE ? h64_load_be32(sys->rdram + paddr) : 0;
        return 0;
    }
    if (paddr < 0x04000000u) { *value = h64_mmio_read(sys, paddr); return 0; }
    if (paddr < 0x04040000u) { *value = h64_load_be32(sys->spMem + (paddr & 0x1FFFu)); return 0; }
    if (paddr < 0x04900000u) { *value = h64_mmio_read(sys, paddr); return 0; }
    if (paddr < 0x10000000u) { *value = paddr >= 0x05000000u ? open_bus(paddr) : 0; return 0; }
    if (paddr < 0x1FC00000u)
    {
        u32 off = paddr - 0x10000000u;
        if (paddr >= ISV_BASE && paddr < ISV_END)
        {
            if (paddr == ISV_BASE) *value = 0x49533634u;   // "IS64": lets software detect the ISViewer
            else if (paddr >= ISV_BASE + 0x20 && paddr < ISV_BASE + 0x20 + sizeof(sys->isvBuffer))
                *value = h64_load_be32(sys->isvBuffer + (paddr - ISV_BASE - 0x20));
            else *value = 0;
            return 0;
        }
        // A CPU write to the cartridge bus is latched by the PI: the next read
        // returns it (once), for ~250 cycles (n64-systemtest cart-writing).
        if (sys->cpu.cycles < sys->pi.latchUntil)
        {
            *value = sys->pi.latch;
            sys->pi.latchUntil = 0;
            return 0;
        }
        *value = off < sys->rom.size ? h64_load_be32(sys->rom.data + off) : open_bus(paddr);
        return 0;
    }
    if (paddr < 0x1FC007C0u) { *value = 0; return 0; }   // PIF boot ROM: not emulated
    if (paddr < 0x1FC00800u) { *value = h64_load_be32(sys->pifRam + (paddr - 0x1FC007C0u)); return 0; }
    *value = 0;
    return 0;
}

int h64_bus_write32(H64System *sys, u32 paddr, u32 value, u32 mask)
{
    paddr &= ~3u;
    if (paddr < 0x03F00000u)
    {
        if (paddr < H64_RDRAM_SIZE)
        {
            u32 old = h64_load_be32(sys->rdram + paddr);
            h64_store_be32(sys->rdram + paddr, (old & ~mask) | (value & mask));
        }
        return 0;
    }
    if (paddr < 0x04000000u) { h64_mmio_write(sys, paddr, value, mask); return 0; }
    if (paddr < 0x04040000u)
    {
        u8 *p = sys->spMem + (paddr & 0x1FFFu);
        u32 old = h64_load_be32(p);
        h64_store_be32(p, (old & ~mask) | (value & mask));
        return 0;
    }
    if (paddr < 0x04900000u) { h64_mmio_write(sys, paddr, value, mask); return 0; }
    if (paddr < 0x10000000u) return 0;   // SRAM/FlashRAM: M5
    if (paddr < 0x1FC00000u)
    {
        if (paddr >= ISV_BASE && paddr < ISV_END)
        {
            if (paddr == ISV_BASE + 0x14)
                isv_flush(sys, value & mask);
            else if (paddr >= ISV_BASE + 0x20 && paddr < ISV_BASE + 0x20 + sizeof(sys->isvBuffer))
            {
                u8 *p = sys->isvBuffer + (paddr - ISV_BASE - 0x20);
                u32 old = h64_load_be32(p);
                h64_store_be32(p, (old & ~mask) | (value & mask));
            }
        }
        // ROM is read-only; the PI latches the value while it is not busy.
        if (sys->cpu.cycles >= sys->pi.latchUntil)
        {
            sys->pi.latch = value;
            sys->pi.latchUntil = sys->cpu.cycles + 250;   // model: decays after ~70 3-instruction loop iterations
        }
        return 0;
    }
    if (paddr < 0x1FC007C0u) return 0;
    if (paddr < 0x1FC00800u)
    {
        u32 off = paddr - 0x1FC007C0u;
        u32 old = h64_load_be32(sys->pifRam + off);
        h64_store_be32(sys->pifRam + off, (old & ~mask) | (value & mask));
        h64_pif_write_byte_hook(sys, off);
        return 0;
    }
    return 0;
}

// Sub-word accesses: RDRAM, RSP memory and PIF RAM are byte-addressable; the
// rest is read as a 32-bit word and the byte lane extracted.
// Cartridge ROM: the PI reads 32 bits from the halfword-aligned address and
// the CPU takes its usual byte lane, so LB at offset 2 returns byte 4
// (n64-systemtest cart: Read8/Read16).
static int cart_sub_read(H64System *sys, u32 paddr, u32 *w)
{
    u32 off = (paddr & ~1u) - 0x10000000u;
    if (sys->cpu.cycles < sys->pi.latchUntil || off + 4 > sys->rom.size || (paddr >= ISV_BASE && paddr < ISV_END))
        return h64_bus_read32(sys, paddr, w);
    *w = h64_load_be32(sys->rom.data + off);
    return 0;
}

int h64_bus_read8(H64System *sys, u32 paddr, u8 *value)
{
    u32 w;
    if (paddr < H64_RDRAM_SIZE) { *value = sys->rdram[paddr]; return 0; }
    if (paddr >= 0x10000000u && paddr < 0x1FC00000u)
    {
        cart_sub_read(sys, paddr, &w);
        *value = (u8)(w >> (8 * (3 - (paddr & 3))));
        return 0;
    }
    if (h64_bus_read32(sys, paddr, &w)) return -1;
    *value = (u8)(w >> (8 * (3 - (paddr & 3))));
    return 0;
}

int h64_bus_read16(H64System *sys, u32 paddr, u16 *value)
{
    u32 w;
    if (paddr < H64_RDRAM_SIZE) { *value = h64_load_be16(sys->rdram + paddr); return 0; }
    if (paddr >= 0x10000000u && paddr < 0x1FC00000u)
    {
        cart_sub_read(sys, paddr, &w);
        *value = (u16)(w >> (8 * (2 - (paddr & 2))));
        return 0;
    }
    if (h64_bus_read32(sys, paddr, &w)) return -1;
    *value = (u16)(w >> (8 * (2 - (paddr & 2))));
    return 0;
}

int h64_bus_read64(H64System *sys, u32 paddr, u64 *value)
{
    u32 hi, lo;
    if (paddr < H64_RDRAM_SIZE - 7) { *value = h64_load_be64(sys->rdram + paddr); return 0; }
    if (h64_bus_read32(sys, paddr, &hi) || h64_bus_read32(sys, paddr + 4, &lo)) return -1;
    *value = ((u64)hi << 32) | lo;
    return 0;
}

// SB/SH: RDRAM honours byte enables. Everywhere else (RSP memory, PIF RAM,
// RCP registers, cartridge) the whole 32-bit word is written with the
// register value shifted into the lane (n64-systemtest spmem/pifram tests),
// so the caller passes the full register value.
int h64_bus_write8(H64System *sys, u32 paddr, u32 regValue)
{
    u32 shift = 8 * (3 - (paddr & 3));
    if (paddr < H64_RDRAM_SIZE) { sys->rdram[paddr] = (u8)regValue; return 0; }
    return h64_bus_write32(sys, paddr, regValue << shift, 0xFFFFFFFFu);
}

int h64_bus_write16(H64System *sys, u32 paddr, u32 regValue)
{
    u32 shift = 8 * (2 - (paddr & 2));
    if (paddr < H64_RDRAM_SIZE) { h64_store_be16(sys->rdram + paddr, (u16)regValue); return 0; }
    return h64_bus_write32(sys, paddr, regValue << shift, 0xFFFFFFFFu);
}

// SD outside RDRAM only writes the upper word (n64-systemtest spmem SD).
int h64_bus_write64(H64System *sys, u32 paddr, u64 value)
{
    if (paddr < H64_RDRAM_SIZE - 7) { h64_store_be64(sys->rdram + paddr, value); return 0; }
    return h64_bus_write32(sys, paddr, (u32)(value >> 32), 0xFFFFFFFFu);
}
