// Harissa64 V2 - front-end configuration (Xbox 360).
//
// Read in layers: defaults, game:\harissa64v2.ini (developer keys and
// scripted runs), then the menu's files: <drive>:\config\settings.ini (all
// games) and <drive>:\config\<game folder>.ini (one game's profile).
#ifndef XB_CONFIG_H
#define XB_CONFIG_H

#include "../../core/common/h64_types.h"
#include "../../core/pif/h64_input_script.h"
#include "../../core/pif/h64_padmap.h"

struct Config
{
    char mode[32];      // mode=play|jittest
    char cpu[32];       // cpu=dynarec|interp
    char rom[256];      // rom=PATH: run this ROM directly (no ROM browser)
    char lastRom[256];  // lastrom=PATH: the ROM browser's selection (written by the menu)
    char romDir[256];   // romdir=FOLDER: the folder the ROM browser lists, ending with a backslash
                        // (settings.ini, chosen in the file browser; default game:\roms\)
    int hle;            // hle=0: RSP tasks on the LLE RSP
    int softRenderer;   // renderer=soft: the software RDP draws into RDRAM, Xenos only shows RDRAM (diagnosis)
    int xenosDebug;     // xenosdebug=1..4: renderer debug output (h64_xenos_set_debug)
    u32 pauseAt;        // pauseat=N: hold the frame shown at VI N for 20 s (window captures in Xenia)
    H64InputScript input;   // input=SCRIPT: scripted controller 1 (h64test --input syntax) instead of the pad
    u32 shots[64];      // shots=f1,f2,...: save the frame shown at these VIs (debug, scripted runs)
    int shotCount;
    u32 exitAfter;      // exitafter=N: return to the dashboard after N VIs (scripted runs)
    u32 stallMs;        // stallms=N: log the frames whose emulation took over N ms ([stall], default 100)
    u32 trace, traceStep;   // trace=N tracestep=C: log N state hashes every C cycles (h64test --trace-frames)
    int fpuFlags;       // fpuflags=0: never read the host FPU flags (FPSCR)
    int jitFpu;         // jitfpu=0: the recompiler leaves COP1 arithmetic to the interpreter
    int regCache;       // regcache=0: no MIPS registers kept in host registers (diagnosis)
    int fpCache;        // fpcache=0: no COP1 registers kept in host FPRs (diagnosis)
    int strict;         // strict=1: no ROM hack tolerance (the console's exact CPU and PI behaviour)
    int statusFast;     // statusfast=0: MTC0 Status always in the interpreter (A/B check)
    int fullExits;      // fullexits=1: the dynarec's older inline linked exits (A/B check)
    int superblocks;    // superblocks=0: blocks end at every branch (A/B check)
    int fastFpu;        // fastfpu=0: exact COP1 (FCR31 cause/flag bits kept); 1 by default
    int smooth;         // smooth=0: no edge smoothing when frames are shown
    int resScale;       // resolution=1|2|3: internal resolution, native 320x240 x1, x2, x3 (default)
    int texFilter;      // texfilter=0|1|2: N64 3-point, bilinear (default), nearest
    int sharpen;        // sharpen=0|1|2: contrast-adaptive sharpening (off, low, high)
    int blur;           // blur=0|1|2: blur (off, soft, strong)
    int screen;         // screen=0..4: none, scanlines, CRT, curved CRT, LCD grid (crt=1: curved CRT)
    int brightness;     // brightness=0|1|2: as the console, lighter, much lighter
    int vsync;          // vsync=1|0: frames wait for the TV's refresh (default 1)
    int deflicker;      // deflicker=0|1: each picture shown mixed with the previous one (default 0)
    int zoom;           // zoom=N: picture size in percent (default 100)
    int offsetX, offsetY;   // posx=, posy=: picture position in output pixels (default 0)
    int stretch;        // stretch=0|1: the picture stretched to 16:9 (any resolution; HD then stretches instead of widening)
    int hdTextures;     // hdtextures=1: the game's hi-res texture pack is used when one is installed (xb_hd.h HdResolve)
    char cheats[256];   // cheats=ID:state,...: the cheats that are on (a game's profile; set by the in-game Cheats page)
    char hdPack[48];    // hdpack=ID: which one (a game's profile; set by the HD textures page): a pack of
                        // <drive>:\hd_textures\packs\, "file" for the game's own file, empty: the first one found
    u32 hdBudget;       // hdbudget=MB: memory for its pictures (default 96)
    char hdCatalog[256];   // hdcatalog=URL: the texture pack catalogue (default: docs/texture-packs.json on GitHub)
    int showFps;        // showfps=1: frames shown per second in a corner; 2: also CPU and RAM; 3: and the resolution
    int logCpu;         // logcpu=N: hardware thread of the log writer (5)
    int gameFlags;      // gameflags=HEX: the game's profile flags instead of the table's (checks; -1: the table)
    int updateCheck;    // updatecheck=0: do not ask GitHub for a newer release (xb_update.cpp); 2: as if running 0.0.0 (testing)
    int artDownload;    // artdownload=0: the ROM browser uses only the box art and game information already in
                        // <drive>:\art\games\ (xb_art.cpp); 1 (default): it downloads what is missing; 2: also in Xenia
    int browserDemo;    // browserdemo=N: the ROM browser moves its selection down every N ms (captures in Xenia)
    char browserSel[128]; // browsersel=FILE: the ROM browser starts on that file name (console checks), over lastrom=
    int filesOpen;      // filesopen=1: the ROM browser opens its file browser at once (console checks)
    int artAll;         // artall=1: the ROM browser starts "Get box art for all games" by itself (checks)
    int asyncGfx;       // asyncgfx=0: graphics tasks and presents on the CPU thread
    int parseWorker;    // parseworker=0: display lists parsed on the graphics worker too (no second worker)
    int asyncAudio;     // asyncaudio=0: audio HLE tasks on the CPU thread
    u32 audioCycles;    // audiocycles=N: CPU cycles an asynchronous audio task keeps the RSP busy
    u32 gfxCycles;      // gfxcycles=N: CPU cycles an asynchronous graphics task keeps the RSP busy
    int gfxTiming;      // gfxtiming=1: graphics tasks last the real microcode's estimated time (at least gfxcycles)
    int lateParse;      // lateparse=0/1: graphics tasks end without waiting for their parse (experimental; -1: per game)
    int cpi;            // cpi=N: CPU cycles per instruction (2 by default; mupen64plus's CountPerOp N is 2N)
    int cpiSet;         // cpi= was in the ini (else a game profile may choose)
    int audioMs;        // audioms=N: audio queue target in ms (pacing threshold)
    int wavDump;        // wavdump=N (diagnosis): the first N seconds of game sound to <drive>:\audio.wav
    int stateSlot;      // stateslot=N: save state slot at start (1..9)
    int loadState;      // loadstate=1: load the slot's state at start (scripted runs)
    u32 saveStateAt;    // savestateat=N: save a state at VI N (scripted runs)
    u32 menuAt;         // menuat=N: open the in-game menu at VI N (remote checks; does not skip the ROM browser)
    u32 aboutAt;        // aboutat=N: the same, showing the About panel
    u32 cheatsAt;       // cheatsat=N: the same, showing the Cheats page (remote checks)
    int cheatOn;        // cheaton=N: cheat N of the list is on from the start (remote checks; -1: none)
    int achievements;   // achievements=1: RetroAchievements on (Settings > Achievements; all games)
    u32 achAt;          // achat=N: the in-game menu opens at VI N on the Achievements page (remote checks)
    int raTest;         // ratest=1: asks the RetroAchievements server for a game id at start-up and logs it
    int hdPage;         // hdpage=1: the ROM browser opens the HD textures page of its selection (checks); 2: and installs the first pack
    int controlsTest;   // controlstest=N: the ROM list opens Settings > Controls on item N; 100+N: also
                        // waiting for an input, again and again; 200+N: the Settings page on item N
                        // (remote checks of the drawing; -1: off)
    int autoStart;      // autostart=N: the ROM browser starts N games by itself, the last selection then the
                        // next ones in the list (remote checks of game changes)
    int autoIndex;      // games the browser started by itself so far
    u32 browserAt;      // browserat=N: back to the ROM list at VI N (as the menu's "Back to the ROM list")
    int xenia;          // xenia=1: running in Xenia (no FPSCR access, no return to the dashboard)
    char lang[8];       // lang=en|es|de|fr|ar|pt|ru: the menus' language (settings.ini; empty: not chosen yet)
    char uiLang[8];     // uilang=CODE (harissa64v2.ini, checks): this language whatever settings.ini says, no language list
    int langPick;       // langpick=1: the language list at start even when a language is set (checks); 2..8: on LANG_* N-2
    int langAuto;       // langauto=MS: the language list confirms its selection by itself after MS ms (checks)
    int rumble;         // rumble=0|1: a Rumble Pak in each controller of the games that support it (default 1)
    H64PadMap padMap[4];    // controls of N64 ports 1-4 (map_a=xa,... ; p2_map_a=... for port 2; core/pif/h64_padmap.h)
};

void ConfigDefaults(Config *c);
// Applies the key=value lines of a file over *c. Returns 0 when the file is missing.
int ConfigParseFile(Config *c, const char *path);
// Writes the keys the menu edits (cpu, hle, audioms, showfps, the graphics
// keys, the controls, and lastrom and lang when `withLastRom`). Returns 0 on failure.
int ConfigWriteMenuKeys(const Config *c, const char *path, int withLastRom);
// Sets lang= in the menu's settings file (the other lines stay; the file is
// made when missing). Returns 0 on failure.
int ConfigWriteLang(const char *path, const char *code);
// Sets key=value in a settings file the same way (the other lines stay).
int ConfigWriteKey(const char *path, const char *key, const char *value);

#endif
