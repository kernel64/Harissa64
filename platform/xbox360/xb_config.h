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
    u32 shots[64];      // shots=f1,f2,...: save the frame shown at these VIs (debug, scripted runs)
    int shotCount;
    u32 exitAfter;      // exitafter=N: return to the dashboard after N VIs (scripted runs)
    u32 trace, traceStep;   // trace=N tracestep=C: log N state hashes every C cycles (h64test --trace-frames)
    int fpuFlags;       // fpuflags=0: never read the host FPU flags (FPSCR)
    int jitFpu;         // jitfpu=0: the recompiler leaves COP1 arithmetic to the interpreter
    int regCache;       // regcache=0: no MIPS registers kept in host registers (diagnosis)
    int fpCache;        // fpcache=0: no COP1 registers kept in host FPRs (diagnosis)
    int fullExits;      // fullexits=1: the dynarec's older inline linked exits (A/B check)
    int superblocks;    // superblocks=0: blocks end at every branch (A/B check)
    int fastFpu;        // fastfpu=0: exact COP1 (FCR31 cause/flag bits kept); 1 by default
    int smooth;         // smooth=0: no edge smoothing when frames are shown
    int resScale;       // resolution=1|2|3: internal resolution, native 320x240 x1, x2, x3 (default)
    int texFilter;      // texfilter=0|1|2: N64 3-point, bilinear (default), nearest
    int sharpen;        // sharpen=0|1|2: contrast-adaptive sharpening (off, low, high)
    int blur;           // blur=0|1|2: blur (off, soft, strong)
    int screen;         // screen=0..4: none, scanlines, CRT, curved CRT, LCD grid (crt=1: curved CRT)
    int aspect;         // aspect=0|1|2: 4:3, 16:9 widescreen (wider 3D), 16:9 stretched
    int showFps;        // showfps=1: frames shown per second in a corner
    int asyncGfx;       // asyncgfx=0: graphics tasks and presents on the CPU thread
    int parseWorker;    // parseworker=0: display lists parsed on the graphics worker too (no second worker)
    int asyncAudio;     // asyncaudio=0: audio HLE tasks on the CPU thread
    u32 audioCycles;    // audiocycles=N: CPU cycles an asynchronous audio task keeps the RSP busy
    u32 gfxCycles;      // gfxcycles=N: CPU cycles an asynchronous graphics task keeps the RSP busy
    int gfxTiming;      // gfxtiming=1: graphics tasks last the real microcode's estimated time (at least gfxcycles)
    int lateParse;      // lateparse=1: graphics tasks end without waiting for their parse (experimental)
    int cpi;            // cpi=N: CPU cycles per instruction (2 by default; mupen64plus's CountPerOp N is 2N)
    int audioMs;        // audioms=N: audio queue target in ms (pacing threshold)
    int stateSlot;      // stateslot=N: save state slot at start (1..9)
    int loadState;      // loadstate=1: load the slot's state at start (scripted runs)
    u32 saveStateAt;    // savestateat=N: save a state at VI N (scripted runs)
    u32 menuAt;         // menuat=N: open the in-game menu at VI N (remote checks; does not skip the ROM browser)
    u32 aboutAt;        // aboutat=N: the same, showing the About panel
    int autoStart;      // autostart=N: the ROM browser starts N games by itself, the last selection then the
                        // next ones in the list (remote checks of game changes)
    int autoIndex;      // games the browser started by itself so far
    u32 browserAt;      // browserat=N: back to the ROM list at VI N (as the menu's "Back to the ROM list")
    int xenia;          // xenia=1: running in Xenia (no FPSCR access, no return to the dashboard)
};

void ConfigDefaults(Config *c);
// Applies the key=value lines of a file over *c. Returns 0 when the file is missing.
int ConfigParseFile(Config *c, const char *path);
// Writes the keys the menu edits (cpu, hle, audioms, showfps, the graphics
// keys, and lastrom when `withLastRom`). Returns 0 on failure.
int ConfigWriteMenuKeys(const Config *c, const char *path, int withLastRom);

#endif
