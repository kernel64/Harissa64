// Harissa64 V2 - front-end configuration (Xbox 360).
//
// Read in layers: defaults, game:\harissa64v2.ini (developer keys and
// scripted runs), then the menu's files: <drive>:\config\settings.ini (all
// games) and <drive>:\config\<game folder>.ini (one game's profile).
#ifndef XB_CONFIG_H
#define XB_CONFIG_H

#include "../../core/common/h64_types.h"
#include "../../core/pif/h64_input_script.h"

struct Config
{
    char mode[32];      // mode=play|jittest
    char cpu[32];       // cpu=dynarec|interp
    char rom[256];      // rom=PATH: run this ROM directly (no ROM browser)
    char lastRom[256];  // lastrom=PATH: the ROM browser's selection (written by the menu)
    int hle;            // hle=0: RSP tasks on the LLE RSP
    int softRenderer;   // renderer=soft: the software RDP draws into RDRAM, Xenos only shows RDRAM (diagnosis)
    int xenosDebug;     // xenosdebug=1..4: renderer debug output (h64_xenos_set_debug)
    u32 pauseAt;        // pauseat=N: hold the frame shown at VI N for 20 s (window captures in Xenia)
    H64InputScript input;   // input=SCRIPT: scripted controller 1 (h64test --input syntax) instead of the pad
    u32 shots[16];      // shots=f1,f2,...: save the frame shown at these VIs (debug, scripted runs)
    int shotCount;
    u32 exitAfter;      // exitafter=N: return to the dashboard after N VIs (scripted runs)
    u32 trace, traceStep;   // trace=N tracestep=C: log N state hashes every C cycles (h64test --trace-frames)
    int fpuFlags;       // fpuflags=0: never read the host FPU flags (FPSCR)
    int jitFpu;         // jitfpu=0: the recompiler leaves COP1 arithmetic to the interpreter
    int regCache;       // regcache=0: no MIPS registers kept in host registers (diagnosis)
    int smooth;         // smooth=0: no edge smoothing when frames are shown
    int showFps;        // showfps=1: frames shown per second in a corner
    int asyncGfx;       // asyncgfx=0: graphics tasks and presents on the CPU thread
    int asyncAudio;     // asyncaudio=0: audio HLE tasks on the CPU thread
    u32 audioCycles;    // audiocycles=N: CPU cycles an asynchronous audio task keeps the RSP busy
    u32 gfxCycles;      // gfxcycles=N: CPU cycles an asynchronous graphics task keeps the RSP busy
    int cpi;            // cpi=N: CPU cycles per instruction (1 by default; mupen64plus's CountPerOp N is 2N)
    int audioMs;        // audioms=N: audio queue target in ms (pacing threshold)
    int stateSlot;      // stateslot=N: save state slot at start (1..9)
    int loadState;      // loadstate=1: load the slot's state at start (scripted runs)
    u32 saveStateAt;    // savestateat=N: save a state at VI N (scripted runs)
    u32 menuAt;         // menuat=N: open the in-game menu at VI N (checks of the menu's look)
    int xenia;          // xenia=1: running in Xenia (no FPSCR access, no return to the dashboard)
};

void ConfigDefaults(Config *c);
// Applies the key=value lines of a file over *c. Returns 0 when the file is missing.
int ConfigParseFile(Config *c, const char *path);
// Writes the keys the menu edits (cpu, hle, audioms, smooth, showfps, and
// lastrom when `withLastRom`). Returns 0 on failure.
int ConfigWriteMenuKeys(const Config *c, const char *path, int withLastRom);

#endif
