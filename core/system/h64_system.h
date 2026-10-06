// Harissa64 V2 - the whole machine: CPU, memories, RCP registers, PIF,
// cartridge and the scheduler that times them.
#ifndef H64_SYSTEM_H
#define H64_SYSTEM_H

#include "../common/h64_types.h"
#include "../cart/h64_rom.h"
#include "../r4300/h64_cpu.h"
#include "../rdp/h64_rdp.h"
#include "../rsp/h64_rsp.h"
#include "../dynarec/h64_jit.h"
#include "../scheduler/h64_scheduler.h"

#define H64_RDRAM_SIZE 0x800000u   // 8 MB (with the Expansion Pak)

// MI interrupt bits (MI_INTR / MI_MASK).
#define MI_INTR_SP 0x01u
#define MI_INTR_SI 0x02u
#define MI_INTR_AI 0x04u
#define MI_INTR_VI 0x08u
#define MI_INTR_PI 0x10u
#define MI_INTR_DP 0x20u

// Controller state (N64 button bits: A 0x8000, B 0x4000, Z 0x2000,
// START 0x1000, D-pad U/D/L/R 0x0800/0x0400/0x0200/0x0100, L 0x0020,
// R 0x0010, C U/D/L/R 0x0008/0x0004/0x0002/0x0001; stick -128..127).
struct H64Pad { u16 buttons; s8 x, y; };

struct H64Mi { u32 mode, version, intr, mask; };
struct H64Vi { u32 regs[14]; u32 vIntr; u64 frameStart; u64 frameCycles; u32 frames; };
struct H64Ai { u32 dramAddr, len, control, status, dacrate, bitrate; u32 fifoLen[2]; u32 fifoCount; u64 bufferCycles; };
struct H64Pi { u32 regs[13]; u32 latch; u64 latchUntil; };   // latch: last CPU write to the cartridge bus
struct H64Ri { u32 regs[8]; };
struct H64Si { u32 dramAddr, pifAddrRd, pifAddrWr, status; };

// Options that change emulated timing: named and documented (CLAUDE.md).
struct H64Options
{
    int hleBoot;        // 1: skip IPL3 and set up its results directly
    int emux;           // 1: answer the EMUX emulator-extension COP0 instructions
                        //    (XDETECT/XLOG/XIOCTL, used by n64-systemtest); 0: NOPs
    int noRdpDraw;      // 1: RDP commands are accepted (syncs, interrupts) but nothing is drawn;
                        //    for long CPU checks such as the recompiler lockstep (not for play)
    int hleAudio;       // 1: audio RSP tasks run in C++ (core/hle) instead of on the LLE RSP
    int rdpStateOnly;   // 1: the software RDP keeps its state and TMEM up to date but draws nothing
                        //    (a GPU renderer draws the primitives from that state)
    int hleGfx;         // 1: graphics RSP tasks run in C++ (core/hle/h64_gfx) when the microcode is known
    int hleAudioCheck;  // 1: audio tasks run on the LLE RSP, and the HLE runs each one on a copy
                        //    of RDRAM; the results are compared when the LLE task ends (debug)
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
    H64Rsp rsp;
    H64RdpRegs dp;
    u64 dpCommand[22];   // RDP command being assembled
    u32 dpPendingWords;
    u64 dpCommands;      // statistics: RDP commands executed
    H64Jit *jit;         // dynamic recompiler (NULL: interpreter)
    struct hle_t *hle;   // RSP task HLE state (allocated at creation)
    struct H64Renderer *renderer;   // NULL: the software RDP draws everything
    struct H64RdpState *rdpState;   // software renderer (allocated on first use)
    u8 *rdramHidden;     // RDRAM's 9th bits: 2 per 16-bit halfword, one byte each (coverage, dz)

    int tvType;          // 0 PAL, 1 NTSC, 2 MPAL
    H64Pad pad[4];       // controller state
    void (*padHook)(H64System *sys);   // optional: called just before the PIF reads the controllers

    // ISViewer debug output (cartridge 0x13FF0000).
    u8 isvBuffer[0x200];
    char isvLine[1024];
    int isvLineLen;
    void (*isvSink)(void *user, const char *line);
    void *isvUser;
    // Audio output: called with each AI buffer as the game queues it
    // (16-bit big-endian stereo samples, `len` bytes, sample rate in Hz).
    void (*aiSink)(void *user, const u8 *samples, u32 len, u32 rate);
    void *aiUser;

    int stop;            // set to leave the run loop
    int exitRequested;   // the guest asked to end the run (EMUX XIOCTL exit)
    u32 miRaised[6];     // statistics: MI interrupts raised, per line (SP SI AI VI PI DP)
};

// Creates a system for a ROM image (any dump order). Returns 0 or -1.
int h64_system_init(H64System *sys, const u8 *romFile, u32 romSize, const H64Options *opt);
void h64_system_free(H64System *sys);
void h64_system_reset(H64System *sys);
// Runs until `cycles` more CPU cycles have elapsed or stop is set.
void h64_system_run_cycles(H64System *sys, u64 cycles);
// Due events, then one interpreter step (the reference side of the lockstep check).
void h64_system_step(H64System *sys);
// Debug: hashes of the CPU registers and of RDRAM, computed from values (the
// same on every host), to compare runs between hosts.
void h64_system_state_hash(const H64System *sys, u32 *cpuHash, u32 *ramHash);

// Every writer of RDRAM tells the recompiler, which drops the blocks of the pages written.
static inline void h64_jit_notify_write(H64System *sys, u32 paddr, u32 len)
{
    if (sys->jit && paddr < H64_RDRAM_SIZE && len &&
        (sys->jit->pageHead[paddr >> 12] || sys->jit->pageHead[((paddr + len - 1) & (H64_RDRAM_SIZE - 1)) >> 12] || len > 4096))
        h64_jit_invalidate(sys, paddr, len);
}

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
int h64_bus_write8(H64System *sys, u32 paddr, u32 regValue);   // full register value (see h64_bus.cpp)
int h64_bus_write16(H64System *sys, u32 paddr, u32 regValue);
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
void h64_pif_read_hook(H64System *sys);   // SI read (PIF -> RDRAM): controllers polled again
void h64_pif_write_byte_hook(H64System *sys, u32 offset);
void h64_hle_boot(H64System *sys);

#endif
