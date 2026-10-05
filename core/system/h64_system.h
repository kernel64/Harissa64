// Harissa64 V2 - the whole machine: CPU, memories, RCP registers, PIF,
// cartridge and the scheduler that times them.
#ifndef H64_SYSTEM_H
#define H64_SYSTEM_H

#include "../common/h64_types.h"
#include "../cart/h64_rom.h"
#include "../r4300/h64_cpu.h"
#include "../scheduler/h64_scheduler.h"

#define H64_RDRAM_SIZE 0x800000u   // 8 MB (with the Expansion Pak)

// MI interrupt bits (MI_INTR / MI_MASK).
#define MI_INTR_SP 0x01u
#define MI_INTR_SI 0x02u
#define MI_INTR_AI 0x04u
#define MI_INTR_VI 0x08u
#define MI_INTR_PI 0x10u
#define MI_INTR_DP 0x20u

// SP_STATUS bits.
#define SP_STATUS_HALT   0x0001u
#define SP_STATUS_BROKE  0x0002u
#define SP_STATUS_DMABUSY 0x0004u
#define SP_STATUS_INTR_BREAK 0x0040u

struct H64Mi { u32 mode, version, intr, mask; };
struct H64Vi { u32 regs[14]; u32 vIntr; u64 frameStart; u64 frameCycles; u32 frames; };
struct H64Ai { u32 dramAddr, len, control, status, dacrate, bitrate; u32 fifoLen[2]; u32 fifoCount; u64 bufferCycles; };
struct H64Pi { u32 regs[13]; };
struct H64Ri { u32 regs[8]; };
struct H64Si { u32 dramAddr, pifAddrRd, pifAddrWr, status; };
struct H64Sp { u32 regs[8]; u32 pc; u32 semaphore; u32 tasks; };
struct H64Dp { u32 regs[8]; };

// Options that change emulated timing: named and documented (CLAUDE.md).
struct H64Options
{
    int hleBoot;        // 1: skip IPL3 and set up its results directly
    int emux;           // 1: answer the EMUX emulator-extension COP0 instructions
                        //    (XDETECT/XLOG/XIOCTL, used by n64-systemtest); 0: NOPs
};

struct H64System
{
    H64Cpu cpu;
    H64Scheduler sched;
    H64Options options;

    u8 *rdram;          // H64_RDRAM_SIZE bytes, N64 byte order
    u8 spMem[0x2000];   // DMEM (0x0000) + IMEM (0x1000)
    u8 pifRam[64];
    H64Rom rom;

    H64Mi mi;
    H64Vi vi;
    H64Ai ai;
    H64Pi pi;
    H64Ri ri;
    H64Si si;
    H64Sp sp;
    H64Dp dp;

    int tvType;          // 0 PAL, 1 NTSC, 2 MPAL

    // ISViewer debug output (cartridge 0x13FF0000).
    u8 isvBuffer[0x200];
    char isvLine[1024];
    int isvLineLen;
    void (*isvSink)(void *user, const char *line);
    void *isvUser;

    int stop;            // set to leave the run loop
    int exitRequested;   // the guest asked to end the run (EMUX XIOCTL exit)
};

// Creates a system for a ROM image (any dump order). Returns 0 or -1.
int h64_system_init(H64System *sys, const u8 *romFile, u32 romSize, const H64Options *opt);
void h64_system_free(H64System *sys);
void h64_system_reset(H64System *sys);
// Runs until `cycles` more CPU cycles have elapsed or stop is set.
void h64_system_run_cycles(H64System *sys, u64 cycles);

// Interrupt lines.
void h64_mi_raise(H64System *sys, u32 bits);
void h64_mi_clear(H64System *sys, u32 bits);

// Physical bus (paddr = 32-bit physical address). Return 0 on success,
// -1 for a bus error (unmapped).
int h64_bus_read32(H64System *sys, u32 paddr, u32 *value);
int h64_bus_write32(H64System *sys, u32 paddr, u32 value, u32 mask);
int h64_bus_read8(H64System *sys, u32 paddr, u8 *value);
int h64_bus_read16(H64System *sys, u32 paddr, u16 *value);
int h64_bus_read64(H64System *sys, u32 paddr, u64 *value);
int h64_bus_write8(H64System *sys, u32 paddr, u8 value);
int h64_bus_write16(H64System *sys, u32 paddr, u16 value);
int h64_bus_write64(H64System *sys, u32 paddr, u64 value);
void h64_debug_text(H64System *sys, const u8 *text, u32 len);

// Device hooks (h64_devices.cpp).
void h64_devices_reset(H64System *sys);
u32 h64_mmio_read(H64System *sys, u32 paddr);
void h64_mmio_write(H64System *sys, u32 paddr, u32 value, u32 mask);
void h64_device_event(H64System *sys, int ev);

// PIF (h64_pif.cpp).
void h64_pif_reset(H64System *sys);
void h64_pif_run_commands(H64System *sys);
void h64_pif_write_byte_hook(H64System *sys, u32 offset);
void h64_hle_boot(H64System *sys);

#endif
