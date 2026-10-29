// Harissa64 V2 - the whole machine: CPU, memories, RCP registers, PIF,
// cartridge and the scheduler that times them.
#ifndef H64_SYSTEM_H
#define H64_SYSTEM_H

#include "../common/h64_types.h"
#include "../cart/h64_rom.h"
#include "../cart/h64_save.h"
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
struct H64Ai
{
    u32 dramAddr, len, control, status, dacrate, bitrate; u32 fifoLen[2]; u32 fifoCount; u64 bufferCycles;
    u64 statQueued, statReads, statReadSum; u32 statBuffers;   // statistics: bytes queued, AI_LEN reads
};
// latch: last CPU write to the cartridge bus; cartOdd: bit 0 of the last PI_CART_ADDR write (options.piOddCart)
struct H64Pi { u32 regs[13]; u32 latch; u64 latchUntil; u32 cartOdd; };
// RI registers, and the registers of the four 2 MB RDRAM modules (8 MB), which
// the 6105 IPL3 programs and probes to size the memory.
#define H64_RDRAM_MODULES 4
struct H64Ri
{
    u32 regs[8];
    u32 rdram[H64_RDRAM_MODULES][10];
    u32 corrupt;   // a module is being calibrated (current enabled, CC = 0): RDRAM reads may return 0
};
struct H64System;
// RDRAM data read while `corrupt` is set: 0 from no module or an uncalibrated one.
int h64_rdram_read_lost(H64System *sys, u32 paddr);
struct H64Si { u32 dramAddr, pifAddrRd, pifAddrWr, status; };

// Profiling (host time spent per subsystem), when the platform sets profClock.
enum
{
    H64_PROF_GFX_HLE = 0,    // graphics tasks run by the HLE, rendering included
    H64_PROF_AUDIO_HLE,      // audio tasks run by the HLE
    H64_PROF_RENDER,         // RDP commands and triangles sent to the renderer (HLE and LLE)
    H64_PROF_RSP_LLE,        // the LLE RSP interpreter
    H64_PROF_JIT_COMPILE,    // recompiler: block compilation
    H64_PROF_HELPER,         // recompiler: instructions run by the interpreter helper (HLE tasks they start excluded)
    H64_PROF_EVENTS,         // scheduler events (VI, AI, SI, PI...; HLE tasks they start excluded)
    H64_PROF_ASYNC_WAIT,     // waiting for the graphics worker (asyncWait)
    H64_PROF_BLOCKS,         // recompiler: inside generated code (helper and events included)
    H64_PROF_TLB,            // recompiler: the dispatcher's work after TLB changes (links, caches)
    H64_PROF_WAIT_TASK,      // inside ASYNC_WAIT: at a graphics task's end
    H64_PROF_WAIT_DPC,       // inside ASYNC_WAIT: before RDP commands sent through DPC_END
    H64_PROF_WAIT_START,     // inside ASYNC_WAIT: before starting a task
    H64_PROF_GFX_SNAPSHOT,   // inside GFX_HLE: copying what a task's texture loads read (prof[H64_PROF_SNAP_BYTES]: bytes)
    H64_PROF_SNAP_BYTES,
    H64_PROF_GFX_VTX,        // inside GFX_HLE: vertex loading (transform, lighting)
    H64_PROF_GFX_NVTX,       // vertices loaded (a count)
    H64_PROF_GFX_LIGHT,      // inside GFX_VTX: Conker's lighting (temporary measure)
    H64_PROF_GFX_NLIGHT,     // lights summed over the vertices lit
    H64_PROF_TMEM_LOADS,     // RDP texture loads (a count)
    H64_PROF_TMEM_WORDS,     // 64-bit words they wrote
    H64_PROF_TMEM_TLUT,      // time in palette loads (temporary measure)
    H64_PROF_TMEM_NTLUT,     // palette loads (a count)
    H64_PROF_TMEM_TIME,      // time in block and tile loads
    H64_PROF_GFX_PARSE,      // asynchronous graphics tasks: their display lists (GFX_HLE then holds their rendering)
    H64_PROF_GFX_TRI,        // inside the display lists: triangles (clipping, projection, output; a measure)
    H64_PROF_GFX_NTRI,       // triangle commands (a count)
    H64_PROF_COUNT
};

// Options that change emulated timing: named and documented (developer notes).
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
    int gfxTiming;      // graphics HLE tasks last h64_gfx_cost's estimate instead of a fixed time (experimental)
    int gfxLateParse;   // asynchronous graphics tasks end without waiting for their display list's parse (it
                        // must be done before the next task starts); the end's flags are the previous task's
                        // (experimental: a game rewriting the list right after the task ends would see it read late)
    int gfxViewportClip;    // graphics HLE (game profile): drop triangles outside the viewport, clip at the
                            // clip ratio's guard band and take clipped vertices' fog from their depth, as the
                            // microcodes do
    int gfxKeepOffscreen;   // 16:9 widescreen: gfxViewportClip off (more than the viewport is shown)
    int gfxOtherModeInit;   // graphics HLE (game profile): each task starts from the microcode's other modes
    int gfxNoCopyBack;      // GPU renderer (game profile): frames it drew are never copied back for texture loads
    int gfxS2dex1;          // graphics HLE (game profile): S2DEX 1.05+ in C++ (else the LLE RSP runs it)
    int gfxCopyBackFull;    // GPU renderer (game profile): copy back only images at least 320 pixels wide
    int gfxRdramCopy;       // GPU renderer (game profile): copy-mode 1:1 rectangles move memory (xenos rdram_copy_rect)
    int gfxCopyBackWhole;   // GPU renderer (game profile): a copy back for a load copies the whole image
    int gfxFrameToRdram;    // GPU renderer (game profile): the picture the VI shows is copied back to RDRAM
    int gfxSmallToRdram;    // GPU renderer (game profile): small images the RDP leaves are copied back to RDRAM
    int gfxSlotByWidth;     // GPU renderer (game profile): one frame slot per address and width
    int gfxBigTexPool;          // GPU renderer (game profile): textures over 512 KB are kept outside the texture arena
    int gfxCopyBackSameWidth;   // GPU renderer (game profile): a texture load copies a GPU image back only when it is
                                // as wide as the image (other loads read data the game keeps in that memory)
    int gfxWsBorder;        // GPU renderer (game profile), 16:9 widescreen: a scissor within 16 pixels of both edges
                            // counts as full width, and fills touching an edge stay there (a border around the picture)
    int piOddCart;          // game profile: a ROM DMA from an odd cartridge address starts at that byte, as the
                            // old PC emulators did (the PI ignores bit 0); SM64 ROM hacks rely on it
    int gfxShortTask;       // graphics worker (game profile): a task keeps the RSP busy H64_HLE_GFX_CYCLES at most
    int gfxNoOldFrame;      // GPU renderer (game profile): never show a frame older than the one on screen
    int gfxSoft8bit;        // GPU renderer (game profile): drawing into 8-bit colour images done by the software RDP
    int gfxSoftTiny;        // GPU renderer (game profile): drawing into colour images at most 16 pixels wide done by the
                            // software RDP
    int gfxModStHalf;       // graphics HLE (game profile): s, t set by G_MODIFYVTX are halved without texture perspective
    int gfxXluNoBias;       // GPU renderer (game profile): no depth bias for Z mode XLU (decal keeps it)
    int gfxCimgFromRdram;   // GPU renderer (game profile): a frame the CPU wrote since the GPU drew it starts from its memory
    int gfxDepthFillToRdram; // GPU renderer (game profile): depth clears also fill the depth image in RDRAM
    int gfxCopyBackAlpha;   // GPU renderer (game profile): 16-bit fills write their alpha bit and copy-backs keep the
                            // GPU's alpha (else always 1): images drawn to be used as cut-out textures
    int gfxDecalStencil;    // GPU renderer (game profile): decals (Z mode 3) tested on both sides of the stored depth
                            // through the stencil (International Superstar Soccer 64's player shadows)
    int gfxSkipZImage;      // GPU renderer (game profile): rectangles and triangles drawn into a colour image at the depth
                            // image's address are not drawn (Mario Tennis's effect sprites: nothing read them)
    int gfxViLineStart;     // GPU renderer (game profile): among the frame slots holding the VI origin, the one where it
                            // falls at the start of a line (NBA in the Zone 2000's two frames side by side in one image)
    int viIntrLine0;        // game profile: the VI interrupt fires when a field starts (half-line 0), whatever
                            // VI_V_INTR's line, as mupen64plus does (FIFA Soccer 64's boot race, h64_profile.cpp)
    u32 idleYieldPc;        // recompiler (game profile): head of an idle thread's osYieldThread loop (0: none)
    u32 idleRunQueue;       // and the physical address of libultra's __osRunQueue (see h64_jit.cpp)
    u32 countWaitPc;        // recompiler (game profile): head of a loop that only polls Count or an empty
                            // message queue (0: none); time moves on there (h64_jit.cpp)
    u32 countWaitStep;      // at most this many CPU cycles at a time (0: up to the next event)
    u32 countWaitQueue;     // physical address of the OSMesgQueue polled (0: none), skipped only while empty
    int gfxTallFrame;       // GPU renderer (game profile): colour images as tall as the game's scissor, not 4:3
    int gfxMaskToTile;      // GPU renderer (game profile): a wrapped axis with a mask wider than the tile repeats the tile
    int gfxKeepShown;       // GPU renderer (game profile): a VI origin in no frame slot keeps the frame on screen
    int gfxRectPoint;       // GPU renderer (game profile): filtered rectangles drawn 1:1 sample whole texels, as the N64 does
    int strictCpu;          // set before the profile: the console's exact behaviour where ROM hacks rely on the PC
                            // emulators' tolerance (h64_apply_profile_flags); test ROMs (h64test --strict, ini strict=1)
    int gfxAlphaRef0Pass;   // GPU renderer (game profile): an alpha test against 0 keeps every pixel, as the RDP does
    int gfxStream;          // graphics HLE (game profile): a display list may branch to itself until the CPU
                            // writes more of it; tasks run on the CPU thread (no graphics worker)
    int gfxCpuWritten;      // deferred rendering (game profile): loads from GPU images the CPU wrote over since
                            // read RDRAM (h64_gfx.cpp, CimgSig)
    int gfxCpuSprites;      // GPU renderer (game profile): what the CPU draws into a frame after the RDP is shown
                            // over the GPU's picture (H64System.cpuSprite)
    int gfxCostLog;     // h64test --gfx-cost-log: log each LLE graphics task's RSP time with its HLE counts
    int hleAudioCheck;  // 1: audio tasks run on the LLE RSP, and the HLE runs each one on a copy
                        //    of RDRAM; the results are compared when the LLE task ends (debug)
    u32 rdpLogFrame;    // h64test --rdp-log N: log the RDP's image, tile, load and rectangle commands during VI frame N (0: off)
};

// A frame's RDRAM when its graphics task was parsed (profile gfxCpuSprites).
struct H64CpuSprite
{
    u32 addr, bytes;     // 0 bytes: unused
    u8 *data;            // malloc'd, up to H64_CPU_SPRITE_MAX bytes
};
#define H64_CPU_SPRITE_MAX (640 * 480 * 2)
#define H64_CPU_SPRITE_MARK 0x0002u   // the 16-bit pixel a frame's memory holds until the CPU draws there

struct H64System
{
    H64Cpu cpu;
    H64Scheduler sched;
    H64Options options;

    u8 *rdram;          // H64_RDRAM_SIZE bytes, N64 byte order
    u8 spMem[0x2000];   // DMEM (0x0000) + IMEM (0x1000)
    u8 pifRam[64];
    H64Rom rom;
    H64SaveMem *save;   // cartridge save memory and Controller Pak (kept across resets)

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
    struct H64DpcAsync *dpcAsync;   // RDP commands sent through DPC_END, run on the graphics worker (h64_rdp.cpp)
    H64Jit *jit;         // dynamic recompiler (NULL: interpreter)
    struct hle_t *hle;   // RSP task HLE state (allocated at creation)
    struct H64Renderer *renderer;   // NULL: the software RDP draws everything
    u64 (*profClock)(void);         // optional: host clock for the profile below
    u64 prof[H64_PROF_COUNT];       // accumulated profClock ticks per H64_PROF_*
    struct H64RdpState *rdpState;   // software renderer (allocated on first use)
    u8 *rdramHidden;     // RDRAM's 9th bits: 2 per 16-bit halfword, one byte each (coverage, dz)

    int tvType;          // 0 PAL, 1 NTSC, 2 MPAL
    H64Pad pad[4];       // controller state, ports 1-4
    u32 padMask;         // bit n: a controller on port n + 1 (0 counts as 1: port 1 only)
    u32 rumblePaks;      // bit n: a Rumble Pak in the controller of port n + 1 (set by the platform;
                         // never port 1 when the game uses a Controller Pak there)
    u8 rumbleMotor[4];   // the motor of each port's Rumble Pak: 1 on (the platform makes the pad vibrate)
    void (*padHook)(H64System *sys);   // optional: called just before the PIF reads the controllers

    // ISViewer debug output (cartridge 0x13FF0000).
    u8 isvBuffer[0x200];
    char isvLine[1024];
    int isvLineLen;
    void (*isvSink)(void *user, const char *line);
    void *isvUser;
    // Audio output: called with each AI buffer as the game queues it
    // (16-bit big-endian stereo samples, `len` bytes, sample rate in Hz).
    // Optional worker (platform): graphics HLE tasks run there while the CPU
    // goes on; asyncWait returns once the job is done. NULL: synchronous.
    // The task's end (SP and DP interrupts) stays at the same emulated time;
    // the CPU waits for the worker there, and before anything else uses the
    // renderer (LLE RDP commands, presenting a frame).
    u32 (*asyncStart)(void *user, void (*job)(void *arg), void *arg);   // returns the job's ticket
    void (*asyncWait)(void *user);                                        // every queued job
    void (*asyncWaitTicket)(void *user, u32 ticket);                      // up to that job
    void *asyncUser;
    // Optional second graphics worker: display lists are parsed there while
    // the first one renders the previous task. Its wait takes the waiting
    // thread (0: the CPU thread, 1: the first worker). NULL: parsed on the first.
    u32 (*asyncParseStart)(void *user, void (*job)(void *arg), void *arg);
    void (*asyncParseWaitTicket)(void *user, u32 ticket, int fromWorker);
    u32 asyncGfxCycles;             // how long an asynchronous graphics task keeps the RSP busy
    // Audio HLE tasks on another worker, the same way (their RDRAM output is
    // complete when the task ends, where the CPU waits for it).
    void (*asyncAudioStart)(void *user, void (*job)(void *arg), void *arg);
    void (*asyncAudioWait)(void *user);
    u32 asyncAudioCycles;
    void (*aiSink)(void *user, const u8 *samples, u32 len, u32 rate);
    void *aiUser;

    int stop;            // set to leave the run loop
    int exitRequested;   // the guest asked to end the run (EMUX XIOCTL exit)
    u32 miRaised[6];     // statistics: MI interrupts raised, per line (SP SI AI VI PI DP)
    // Profile gfxCpuSprites: the RDRAM of the frames the last graphics tasks
    // drew, as it was when they were parsed (h64_gfx.cpp). The renderer shows
    // the pixels the CPU changed since over the GPU's picture (Dr. Mario
    // draws its thrown capsules into the frame with the CPU).
    H64CpuSprite cpuSprite[4];
    u32 cpuSpriteNext;
    H64RspDecode rspDecode;
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

static inline u64 h64_prof_now(const H64System *sys) { return sys->profClock ? sys->profClock() : 0; }

// HLE and LLE RSP time spent on the CPU thread, to leave out of what a caller
// measures (the workers add theirs to the same counters while the CPU runs).
static inline u64 h64_prof_nested(const H64System *sys)
{
    return (sys->asyncStart ? 0 : sys->prof[H64_PROF_GFX_HLE]) + (sys->asyncAudioStart ? 0 : sys->prof[H64_PROF_AUDIO_HLE]) +
           sys->prof[H64_PROF_RSP_LLE];
}

// Adds the time since t0 minus the nested time since `nested` (h64_prof_nested at t0) to prof[slot].
static inline void h64_prof_add_outer(H64System *sys, int slot, u64 t0, u64 nested)
{
    u64 spent = h64_prof_now(sys) - t0, inner = h64_prof_nested(sys) - nested;
    sys->prof[slot] += spent > inner ? spent - inner : 0;
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
