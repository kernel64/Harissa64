// Harissa64 V2 - Xbox 360 entry point.
//
// Start-up: unit tests on the console's own CPU, then, depending on
// game:\harissa64v2.ini (key=value lines, all optional):
//   mode=play      (default) run a ROM: recompiler or interpreter CPU, RSP
//                  task HLE (graphics and audio), Xenos renderer, XAudio2,
//                  XInput. The ROM is rom=<path>, else game:\test.z64, else
//                  the first .z64/.n64/.v64 in game:\roms\.
//   mode=jittest   the M3 recompiler check (lockstep and timings, no video).
//   cpu=dynarec    (default) or cpu=interp (Xenia cannot run generated code).
//   hle=1          (default) or hle=0 for the LLE RSP (slow; no video yet:
//                  the Xenos renderer only draws HLE triangles).
// The log goes to game:\harissa64v2.log, or cache:\ when game:\ is
// read-only (Xenia). BACK + START returns to the dashboard.
#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ppcintrinsics.h>

#include "../../core/common/h64_types.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../core/common/h64_fenv.h"
#include "../../core/system/h64_system.h"
#include "../../core/dynarec/h64_lockstep.h"
#include "../../core/pif/h64_input_script.h"
#include "../../render/xenos/h64_xenos.h"
#include "../../tests/unit/unit_tests.h"
#include "font8x8_basic.h"
#include "xb_audio.h"

// ---- Recompiler code memory (M0.3 result: only an image section linked
// writable + executable runs generated code; /SECTION:.jitc,ERW in the project) ----
extern "C" VOID NTAPI KeSweepIcacheRange(PVOID Address, SIZE_T Size);

#define JIT_BYTES (16u * 1024u * 1024u)
#pragma section(".jitc", read, write, execute)
__declspec(allocate(".jitc")) static unsigned int s_jitMem[JIT_BYTES / 4] = { 1 };

static void FlushIcache(void *p, u32 size)
{
    u32 i;
    for (i = 0; i < size; i += 128)
        __dcbst((int)i, p);
    __sync();
    KeSweepIcacheRange(p, size);
    __emit(0x4C00012C);   // isync
}

// Speed of the code memory: a loop of 16 nops generated at the end of
// the code cache, against the same loop compiled into .text. A large ratio
// would mean the .jitc pages run uncached.
typedef u32 (*BenchFn)(u32 iterations);

__declspec(noinline) static u32 BenchText(u32 n)
{
    u32 v = 0;
    do
    {
        __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
        __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
        __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
        __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
        v++;
    } while (v < n);
    return v;
}

static void BenchCodeMemory(void)
{
    u32 *code = s_jitMem + (JIT_BYTES - 4096) / 4, i, k = 0;
    LARGE_INTEGER f, a, b, c;
    BenchFn fn = (BenchFn)(void *)code;
    code[k++] = 0x38800000;                       // li r4, 0
    code[k++] = 0x7C6903A6;                       // mtctr r3
    for (i = 0; i < 16; i++) code[k++] = 0x60000000;   // nop (both loops: instruction fetch speed)
    code[k++] = 0x4200FFC0;                       // bdnz -64
    code[k++] = 0x7C832378;                       // mr r3, r4
    code[k++] = 0x4E800020;                       // blr
    FlushIcache(code, k * 4);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    fn(100000);
    QueryPerformanceCounter(&b);
    BenchText(100000);
    QueryPerformanceCounter(&c);
    H64_INFO("[jit] code memory: 1.6M nops in %.2f ms (generated, .jitc) vs %.2f ms (.text)",
             (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart, (double)(c.QuadPart - b.QuadPart) * 1000.0 / f.QuadPart);
}

static FILE *s_log = NULL;
static const char *s_logDrive = "game";   // where the log (and screenshots) go

static void file_sink(int level, const char *line)
{
    (void)level;
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    if (s_log)
    {
        fprintf(s_log, "%s\n", line);
        fflush(s_log);
    }
}

// ---- Configuration ----
struct Config
{
    char mode[32];
    char cpu[32];
    char rom[256];
    int hle;
    int softRenderer;
    int xenosDebug;
    u32 pauseAt;
    H64InputScript input;   // input=SCRIPT: scripted controller 1 (h64test --input syntax) instead of the pad        // pauseat=N: hold the frame shown at VI N for 20 s (window captures in Xenia)     // xenosdebug=1..3: renderer debug output (h64_xenos_set_debug)   // renderer=soft: the software RDP draws into RDRAM, Xenos only shows RDRAM (diagnosis)
    u32 shots[16];      // shots=f1,f2,...: save the frame shown at these VIs (debug, scripted runs)
    int shotCount;
    u32 exitAfter;      // exitafter=N: return to the dashboard after N VIs (scripted runs)
    u32 trace, traceStep;
    int fpuFlags;       // fpuflags=0: never read the host FPU flags (FPSCR)
    int jitFpu;         // jitfpu=0: the recompiler leaves COP1 arithmetic to the interpreter
    int regCache;       // regcache=0: no MIPS registers kept in host registers (diagnosis)
    int cpi;            // cpi=N: CPU cycles per instruction (1 by default; mupen64plus's CountPerOp N is 2N)
    int xenia;          // xenia=1: running in Xenia (no FPSCR access, no return to the dashboard)   // trace=N tracestep=C: log N state hashes every C cycles (h64test --trace-frames)
};

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static void LoadConfig(Config *c)
{
    FILE *f = fopen("game:\\harissa64v2.ini", "r");
    char line[512];
    strcpy(c->mode, "play");
    strcpy(c->cpu, "dynarec");
    c->rom[0] = 0;
    c->hle = 1;
    c->shotCount = 0;
    c->softRenderer = 0;
    c->xenosDebug = 0;
    c->pauseAt = 0;
    memset(&c->input, 0, sizeof(c->input));
    c->trace = c->traceStep = 0;
    c->xenia = 0;
    c->fpuFlags = 1;
    c->jitFpu = 1;
    c->regCache = 1;
    c->cpi = 1;
    c->exitAfter = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        trim(line);
        if (line[0] == '#' || line[0] == ';' || !eq) continue;
        *eq = 0;
        if (!strcmp(line, "mode")) { strncpy(c->mode, eq + 1, sizeof(c->mode) - 1); c->mode[sizeof(c->mode) - 1] = 0; }
        else if (!strcmp(line, "cpu")) { strncpy(c->cpu, eq + 1, sizeof(c->cpu) - 1); c->cpu[sizeof(c->cpu) - 1] = 0; }
        else if (!strcmp(line, "rom")) { strncpy(c->rom, eq + 1, sizeof(c->rom) - 1); c->rom[sizeof(c->rom) - 1] = 0; }
        else if (!strcmp(line, "hle")) c->hle = atoi(eq + 1);
        else if (!strcmp(line, "exitafter")) c->exitAfter = (u32)atoi(eq + 1);
        else if (!strcmp(line, "renderer")) c->softRenderer = !strcmp(eq + 1, "soft");
        else if (!strcmp(line, "trace")) c->trace = (u32)atoi(eq + 1);
        else if (!strcmp(line, "xenia")) c->xenia = atoi(eq + 1);
        else if (!strcmp(line, "fpuflags")) c->fpuFlags = atoi(eq + 1);
        else if (!strcmp(line, "jitfpu")) c->jitFpu = atoi(eq + 1);
        else if (!strcmp(line, "regcache")) c->regCache = atoi(eq + 1);
        else if (!strcmp(line, "cpi")) c->cpi = atoi(eq + 1);
        else if (!strcmp(line, "xenosdebug")) c->xenosDebug = atoi(eq + 1);
        else if (!strcmp(line, "pauseat")) c->pauseAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "input")) h64_input_script_parse(&c->input, eq + 1);
        else if (!strcmp(line, "tracestep")) c->traceStep = (u32)strtoul(eq + 1, NULL, 10);
        else if (!strcmp(line, "shots"))
        {
            char *p = eq + 1;
            while (*p && c->shotCount < 16)
            {
                c->shots[c->shotCount++] = (u32)strtoul(p, &p, 10);
                while (*p == ',' || *p == ' ') p++;
            }
        }
    }
    fclose(f);
}

// ---- Text through Clear rectangles ----
#define MAX_RECTS 4096
static D3DRECT s_rects[MAX_RECTS];
static int s_rectCount;

static void AddText(int x, int y, int scale, const char *text)
{
    int cx = x;
    for (; *text; text++)
    {
        unsigned char c = (unsigned char)*text;
        int row, col;
        if (c == '\n') { y += 10 * scale; cx = x; continue; }
        if (c >= 128) c = '?';
        for (row = 0; row < 8; row++)
        {
            unsigned char bits = font8x8_basic[c][row];
            for (col = 0; col < 8; col++)
            {
                if ((bits & (1 << col)) && s_rectCount < MAX_RECTS)   // bit 0 = leftmost pixel
                {
                    D3DRECT *r = &s_rects[s_rectCount++];
                    r->x1 = cx + col * scale;
                    r->y1 = y + row * scale;
                    r->x2 = r->x1 + scale;
                    r->y2 = r->y1 + scale;
                }
            }
        }
        cx += 8 * scale;
    }
}

static void FlushText(IDirect3DDevice9 *dev, D3DCOLOR color)
{
    if (s_rectCount)
        dev->Clear(s_rectCount, s_rects, D3DCLEAR_TARGET, color, 1.0f, 0);
    s_rectCount = 0;
}

// Shows a message until BACK is pressed, then returns to the dashboard.
static void MessageScreen(IDirect3DDevice9 *dev, const char *title, const char *text, D3DCOLOR color)
{
    for (;;)
    {
        XINPUT_STATE in;
        if (dev)
        {
            dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(14, 16, 26), 1.0f, 0);
            AddText(96, 80, 6, "HARISSA64 V2");
            FlushText(dev, D3DCOLOR_XRGB(220, 40, 30));
            AddText(100, 170, 3, title);
            FlushText(dev, D3DCOLOR_XRGB(230, 230, 230));
            AddText(100, 240, 2, text);
            FlushText(dev, color);
            AddText(100, 640, 2, "Press BACK to return to the dashboard");
            FlushText(dev, D3DCOLOR_XRGB(140, 140, 160));
            dev->Present(NULL, NULL, NULL, NULL);
        }
        if (XInputGetState(0, &in) == ERROR_SUCCESS && (in.Gamepad.wButtons & XINPUT_GAMEPAD_BACK))
            break;
        Sleep(16);
    }
}

// ---- ROM files ----
static u8 *LoadFile(const char *path, u32 *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0, len;
    u8 *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    len = GetFileSize(h, NULL);
    buf = (u8 *)malloc(len ? len : 1);
    if (buf && !ReadFile(h, buf, len, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!buf || got != len) { free(buf); return NULL; }
    *size = len;
    return buf;
}

static int FindRom(const Config *c, char *path, size_t len)
{
    static const char *patterns[] = { "game:\\roms\\*.z64", "game:\\roms\\*.n64", "game:\\roms\\*.v64" };
    WIN32_FIND_DATAA fd;
    int i;
    if (c->rom[0] && GetFileAttributesA(c->rom) != 0xFFFFFFFF) { strncpy(path, c->rom, len); path[len - 1] = 0; return 1; }
    if (GetFileAttributesA("game:\\test.z64") != 0xFFFFFFFF) { strncpy(path, "game:\\test.z64", len); return 1; }
    for (i = 0; i < 3; i++)
    {
        HANDLE f = FindFirstFileA(patterns[i], &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        _snprintf(path, len, "game:\\roms\\%s", fd.cFileName);
        path[len - 1] = 0;
        FindClose(f);
        return 1;
    }
    return 0;
}

static int s_jitNoFpu;   // jitfpu=0
static int s_noRegCache; // regcache=0
static u32 s_cpi = 1;    // cpi=N

// ---- N64 PC sampler (diagnosis): a thread on another hardware thread reads
// the emulated PC every millisecond; [pc] lines give the hottest addresses
// (busy-wait loops the recompiler does not skip show up there).
#define PC_SLOTS 1024
static volatile const u64 *s_samplePc;
static volatile LONG s_samplerOn;
static u32 s_pcKey[PC_SLOTS], s_pcCount[PC_SLOTS], s_pcSamples;

static DWORD WINAPI PcSamplerThread(LPVOID)
{
    while (s_samplerOn)
    {
        u32 pc, h, n;
        Sleep(1);
        if (!s_samplePc) continue;
        pc = (u32)*s_samplePc;
        h = (pc >> 2) & (PC_SLOTS - 1);
        for (n = 0; n < 8; n++, h = (h + 1) & (PC_SLOTS - 1))
            if (s_pcCount[h] == 0 || s_pcKey[h] == pc) { s_pcKey[h] = pc; s_pcCount[h]++; break; }
        s_pcSamples++;
    }
    return 0;
}

static void PcSamplerStart(H64System *sys)
{
    HANDLE h;
    s_samplePc = &sys->cpu.pc;
    if (s_samplerOn) return;
    s_samplerOn = 1;
    h = CreateThread(NULL, 0, PcSamplerThread, NULL, CREATE_SUSPENDED, NULL);
    if (!h) { s_samplerOn = 0; return; }
    XSetThreadProcessor(h, 5);
    ResumeThread(h);
    CloseHandle(h);
}

static void PcSamplerReport(void)
{
    u32 best[4] = { 0, 0, 0, 0 }, bestPc[4] = { 0, 0, 0, 0 }, i, k, total = s_pcSamples;
    char line[160];
    if (!total) return;
    for (i = 0; i < PC_SLOTS; i++)
        for (k = 0; k < 4; k++)
            if (s_pcCount[i] > best[k])
            {
                u32 m;
                for (m = 3; m > k; m--) { best[m] = best[m - 1]; bestPc[m] = bestPc[m - 1]; }
                best[k] = s_pcCount[i];
                bestPc[k] = s_pcKey[i];
                break;
            }
    _snprintf(line, sizeof(line), "[pc] %u samples: %08X %u%%, %08X %u%%, %08X %u%%, %08X %u%%", total,
              bestPc[0], best[0] * 100 / total, bestPc[1], best[1] * 100 / total, bestPc[2], best[2] * 100 / total,
              bestPc[3], best[3] * 100 / total);
    line[sizeof(line) - 1] = 0;
    H64_INFO("%s", line);
    memset(s_pcCount, 0, sizeof(s_pcCount));
    s_pcSamples = 0;
}

static H64System *MakeSystem(const u8 *rom, u32 size, int jit)
{
    H64System *sys = (H64System *)malloc(sizeof(H64System));
    if (!sys || h64_system_init(sys, rom, size, 0)) { free(sys); return NULL; }
    if (jit && h64_jit_init(sys, s_jitMem, JIT_BYTES, FlushIcache))
    {
        H64_ERROR("[jit] h64_jit_init failed");
        h64_system_free(sys);
        free(sys);
        return NULL;
    }
    if (sys->jit) sys->jit->noFpu = s_jitNoFpu;
    if (sys->jit) sys->jit->noRegCache = s_noRegCache;
    sys->cpu.cpi = s_cpi;
    return sys;
}

static void FreeSystem(H64System *sys)
{
    if (!sys) return;
    h64_system_free(sys);
    free(sys);
}

// ---- mode=jittest: the M3 recompiler check ----
// 10 emulated seconds with the recompiler in lockstep with the interpreter,
// then 10 seconds with each alone, without RDP drawing (CPU and RSP only).
static void RunDynarecTest(const Config *c, char *report, size_t len)
{
    char rom[256];
    u8 *data;
    u32 size = 0;
    const u64 cycles = 10ull * 93750000ull;
    H64System *ref, *jit;
    H64LockstepResult ls;
    DWORD t0, tLock, tInterp, tJit;

    if (!FindRom(c, rom, sizeof(rom)))
    {
        _snprintf(report, len, "Recompiler test: no ROM\n(put one at game:\\test.z64 or in game:\\roms\\)");
        report[len - 1] = 0;
        return;
    }
    data = LoadFile(rom, &size);
    if (!data) { _snprintf(report, len, "Recompiler test: cannot read %s", rom); report[len - 1] = 0; return; }
    H64_INFO("[jit] test ROM %s (%u bytes)", rom, size);

    ref = MakeSystem(data, size, 0);
    jit = MakeSystem(data, size, 1);
    if (!ref || !jit) { _snprintf(report, len, "Recompiler test: cannot create the systems"); report[len - 1] = 0; return; }
    ref->options.noRdpDraw = jit->options.noRdpDraw = 1;
    t0 = GetTickCount();
    h64_lockstep_run(ref, jit, cycles, &ls);
    tLock = GetTickCount() - t0;
    if (ls.diverged)
        H64_ERROR("[jit] LOCKSTEP DIVERGENCE after dispatch %llu at pc %08X (cycle %llu, frame %u): %s",
                  (unsigned long long)ls.dispatches, ls.pc, (unsigned long long)ls.cyclesBefore, ls.frame, ls.why);
    H64_INFO("[jit] lockstep %s: %llu dispatches, %llu blocks compiled, %llu idle instructions skipped, %u ms",
             ls.diverged ? "DIVERGED" : "OK", (unsigned long long)ls.dispatches,
             (unsigned long long)jit->jit->stats.blocksCompiled, (unsigned long long)jit->jit->stats.idleSkipped, tLock);
    FreeSystem(ref);
    FreeSystem(jit);

    ref = MakeSystem(data, size, 0);
    ref->options.noRdpDraw = 1;
    t0 = GetTickCount();
    h64_system_run_cycles(ref, cycles);
    tInterp = GetTickCount() - t0;
    FreeSystem(ref);
    jit = MakeSystem(data, size, 1);
    jit->options.noRdpDraw = 1;
    t0 = GetTickCount();
    h64_system_run_cycles(jit, cycles);
    tJit = GetTickCount() - t0;
    H64_INFO("[jit] 10 emulated seconds: interpreter %u ms, recompiler %u ms (%llu native instructions compiled, %llu through the interpreter)",
             tInterp, tJit, (unsigned long long)jit->jit->stats.nativeInsns, (unsigned long long)jit->jit->stats.helperInsns);
    FreeSystem(jit);
    free(data);

    _snprintf(report, len, "Recompiler, 10 emulated s of %s:\nlockstep %s (%u ms)%s%s\ninterpreter %u ms, recompiler %u ms",
              rom + 6, ls.diverged ? "DIVERGED" : "OK", tLock, ls.diverged ? "\n" : "", ls.diverged ? ls.why : "", tInterp, tJit);
    report[len - 1] = 0;
}

// ---- mode=play ----
static XINPUT_STATE s_pad;
static int s_padValid;
static const H64InputScript *s_script;   // scripted input (input=), NULL: the controller

static s8 StickAxis(SHORT v)
{
    int x = v;
    const int dead = 7000;
    if (x > -dead && x < dead) return 0;
    x = x > 0 ? (x - dead) * 80 / (32767 - dead) : (x + dead) * 80 / (32768 - dead);
    return (s8)(x > 80 ? 80 : x < -80 ? -80 : x);
}

// Called by the PIF just before the game reads the controllers.
static void PadHook(H64System *sys)
{
    const XINPUT_GAMEPAD *g = &s_pad.Gamepad;
    u16 b = 0;
    if (s_script) { h64_input_script_apply(s_script, sys); return; }
    if (!s_padValid) { sys->pad[0].buttons = 0; sys->pad[0].x = sys->pad[0].y = 0; return; }
    if (g->wButtons & XINPUT_GAMEPAD_A) b |= 0x8000;
    if (g->wButtons & XINPUT_GAMEPAD_B) b |= 0x4000;
    if ((g->wButtons & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y)) || g->bLeftTrigger > 64 || g->bRightTrigger > 64) b |= 0x2000;
    if (g->wButtons & XINPUT_GAMEPAD_START) b |= 0x1000;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) b |= 0x0800;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) b |= 0x0400;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) b |= 0x0200;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) b |= 0x0100;
    if (g->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= 0x0020;
    if (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= 0x0010;
    if (g->sThumbRY > 16000) b |= 0x0008;
    if (g->sThumbRY < -16000) b |= 0x0004;
    if (g->sThumbRX < -16000) b |= 0x0002;
    if (g->sThumbRX > 16000) b |= 0x0001;
    sys->pad[0].buttons = b;
    sys->pad[0].x = StickAxis(g->sThumbLX);
    sys->pad[0].y = StickAxis(g->sThumbLY);
}

static u64 ProfClock(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}

static int s_crashed;
static char s_crashText[256];

// Runs one VI's worth of cycles under the crash handler.
static int RunFrame(H64System *sys)
{
    __try
    {
        h64_system_run_cycles(sys, sys->vi.frameCycles);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        _snprintf(s_crashText, sizeof(s_crashText), "Exception %08X at N64 pc %08X", GetExceptionCode(),
                  (u32)sys->cpu.pc);
        s_crashText[sizeof(s_crashText) - 1] = 0;
        H64_ERROR("[crash] %s", s_crashText);
        s_crashed = 1;
        return -1;
    }
    return 0;
}

static void RunGame(IDirect3DDevice9 *dev, const Config *c)
{
    char rom[256];
    u8 *data;
    u32 size = 0;
    int jit = !strcmp(c->cpu, "dynarec");
    H64System *sys;
    H64Renderer *renderer = NULL;
    LARGE_INTEGER freq, now, frameStart;
    DWORD perfStart = GetTickCount();
    u32 framesSincePerf = 0, presented = 0;
    LARGE_INTEGER t0, t1, t2;
    LONGLONG profRun = 0, profPresent = 0, profWait = 0;
    u64 jitBlocks = 0, jitInval = 0, jitFlush = 0, jitHelper = 0, jitRun = 0, jitIdle = 0, jitSteps = 0, insnsPrev = 0;
    u64 instrAtPerf = 0;

    if (!FindRom(c, rom, sizeof(rom)))
    {
        MessageScreen(dev, "No ROM found", "Put a ROM at game:\\test.z64 or in game:\\roms\\", D3DCOLOR_XRGB(240, 200, 60));
        return;
    }
    data = LoadFile(rom, &size);
    if (!data) { MessageScreen(dev, "Cannot read the ROM", rom, D3DCOLOR_XRGB(240, 60, 60)); return; }
    sys = MakeSystem(data, size, jit);
    free(data);
    if (!sys) { MessageScreen(dev, "Not an N64 ROM", rom, D3DCOLOR_XRGB(240, 60, 60)); return; }
    sys->options.hleGfx = c->hle;
    sys->options.hleAudio = c->hle;
    sys->padHook = PadHook;
    sys->profClock = ProfClock;
    s_script = c->input.count ? &c->input : NULL;
    if (xb_audio_init() == 0)
    {
        sys->aiSink = xb_audio_sink;
        sys->aiUser = NULL;
    }
    renderer = h64_xenos_create(sys, dev);
    if (!renderer) { MessageScreen(dev, "Renderer initialisation failed", "See harissa64v2.log", D3DCOLOR_XRGB(240, 60, 60)); return; }
    if (c->xenosDebug) h64_xenos_set_debug(renderer, c->xenosDebug);
    if (c->softRenderer) sys->options.rdpStateOnly = 0;
    else sys->renderer = renderer;
    H64_INFO("[main] running %s: cpu %s, RSP %s, %s renderer", rom, jit ? "recompiler" : "interpreter",
             c->hle ? "HLE" : "LLE", c->softRenderer ? "software" : "Xenos");

    if (c->trace)
    {
        u32 n;
        if (c->traceStep >= 1000000000u)
        {
            // Instruction trace (h64test --trace-step 1000000000+START --trace-frames N).
            h64_system_run_cycles(sys, c->traceStep - 1000000000u);
            for (n = 0; n < c->trace; n++)
            {
                u32 ch, pc = (u32)sys->cpu.pc;
                h64_system_step(sys);
                h64_system_state_hash(sys, &ch, NULL);
                H64_INFO("[itrace] %u pc=%08X cyc=%llu cpu=%08X", n, pc, (unsigned long long)sys->cpu.cycles, ch);
            }
        }
        else
        {
            u64 step = c->traceStep ? c->traceStep : sys->vi.frameCycles;
            for (n = 0; n < c->trace; n++)
            {
                u32 ch, rh;
                h64_system_run_cycles(sys, step);
                h64_system_state_hash(sys, &ch, &rh);
                H64_INFO("[trace] %u cycles=%llu instr=%llu pc=%08X cpu=%08X ram=%08X", n,
                         (unsigned long long)sys->cpu.cycles, (unsigned long long)sys->cpu.instructions, (u32)sys->cpu.pc,
                         ch, rh);
            }
        }
        H64_INFO("[main] exitafter reached (trace done)");
        sys->renderer = NULL;
        h64_xenos_free(renderer);
        xb_audio_shutdown();
        s_samplePc = NULL;
        Sleep(5);
        FreeSystem(sys);
        return;
    }
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&frameStart);
    if (!c->xenia) BenchCodeMemory();
    PcSamplerStart(sys);
    for (;;)
    {
        int queued;
        if (XInputGetState(0, &s_pad) == ERROR_SUCCESS) s_padValid = 1;
        else s_padValid = 0;
        if (s_padValid && (s_pad.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) && (s_pad.Gamepad.wButtons & XINPUT_GAMEPAD_START))
            break;
        QueryPerformanceCounter(&t0);
        if (RunFrame(sys)) break;
        QueryPerformanceCounter(&t1);
        h64_xenos_present(renderer);
        QueryPerformanceCounter(&t2);
        profRun += t1.QuadPart - t0.QuadPart;
        profPresent += t2.QuadPart - t1.QuadPart;
        framesSincePerf++;
        presented++;
        {
            int i;
            for (i = 0; i < c->shotCount; i++)
                if (c->shots[i] == presented)
                {
                    char path[64];
                    _snprintf(path, sizeof(path), "%s:\\shot_%05u.bmp", s_logDrive, presented);
                    path[sizeof(path) - 1] = 0;
                    H64_INFO("[main] screenshot %s: %s", path, h64_xenos_save_frame(renderer, path) ? "failed" : "saved");
                }
        }
        if (c->pauseAt && presented == c->pauseAt)
        {
            int k;
            H64_INFO("[main] paused at VI %u", presented);
            for (k = 0; k < 400; k++) { Sleep(50); h64_xenos_present(renderer); }
            H64_INFO("[main] resumed");
        }
        if (c->exitAfter && presented >= c->exitAfter)
        {
            H64_INFO("[main] exitafter reached (%u VIs)", presented);
            break;
        }

        // Pacing: on the audio queue, or on the clock when no sound plays.
        QueryPerformanceCounter(&t0);
        queued = xb_audio_queued_ms();
        if (queued >= 0)
        {
            while (queued > XB_AUDIO_MAX_QUEUED_MS)
            {
                Sleep(1);
                queued = xb_audio_queued_ms();
                if (queued < 0) break;
            }
            QueryPerformanceCounter(&frameStart);
        }
        else
        {
            LONGLONG frameTicks = freq.QuadPart * sys->vi.frameCycles / 93750000;
            for (;;)
            {
                QueryPerformanceCounter(&now);
                if (now.QuadPart - frameStart.QuadPart >= frameTicks) break;
                if (frameTicks - (now.QuadPart - frameStart.QuadPart) > freq.QuadPart / 1000) Sleep(1);
            }
            frameStart.QuadPart += frameTicks;
            if (now.QuadPart - frameStart.QuadPart > frameTicks * 4) frameStart = now;   // far behind: do not catch up
        }

        QueryPerformanceCounter(&t1);
        profWait += t1.QuadPart - t0.QuadPart;
        if (GetTickCount() - perfStart >= 2000)
        {
            DWORD ms = GetTickCount() - perfStart;
            H64XenosStats xs;
            XbAudioStats as;
            h64_xenos_stats(renderer, &xs, 1);
            xb_audio_stats(&as, 1);
            H64_INFO("[perf] vi/s=%.1f mips=%.1f tris=%u rects=%u fills=%u texup=%u (new %u) shaders=%u copyback=%u fbswitch=%u audio=%u buffers %u underruns",
                     framesSincePerf * 1000.0 / ms, (double)(sys->cpu.instructions - instrAtPerf) / (ms * 1000.0),
                     xs.triangles, xs.rects, xs.fills, xs.textureUploads, xs.textureCreates, xs.shaderCompiles, xs.copyBacks, xs.fbSwitches, as.buffers, as.underruns);
            {
                MEMORYSTATUS ms;
                GlobalMemoryStatus(&ms);
                H64_INFO("[mem] free %u KB of %u KB", (u32)(ms.dwAvailPhys / 1024), (u32)(ms.dwTotalPhys / 1024));
            }
            H64_INFO("[state] pc=%08X frames=%u rsp tasks=%u (hle %u) irq SP %u SI %u AI %u VI %u PI %u DP %u mi intr=%02X mask=%02X origin=%06X",
                     (u32)sys->cpu.pc, sys->vi.frames, sys->rsp.tasks, sys->rsp.hleTasks, sys->miRaised[0],
                     sys->miRaised[1], sys->miRaised[2], sys->miRaised[3], sys->miRaised[4], sys->miRaised[5],
                     sys->mi.intr, sys->mi.mask, sys->vi.regs[1] & 0xFFFFFF);
            if (framesSincePerf)
            {
                // Milliseconds per frame. cpu = the frame run minus the subsystems below it
                // (render is part of gfx for HLE tasks, of rsp for LLE ones).
                double k = 1000.0 / (double)freq.QuadPart / framesSincePerf;
                u64 *pr = sys->prof;
                double run = profRun * k, gfx = pr[H64_PROF_GFX_HLE] * k, aud = pr[H64_PROF_AUDIO_HLE] * k;
                double rsp = pr[H64_PROF_RSP_LLE] * k, jitc = pr[H64_PROF_JIT_COMPILE] * k;
                const H64JitStats *js = sys->jit ? &sys->jit->stats : NULL;
                H64_INFO("[prof] ms/frame: run %.1f (cpu %.1f, gfx hle %.1f incl. render %.1f, audio hle %.1f, rsp lle %.1f, "
                         "jit compile %.1f) present %.1f wait %.1f | jit +%llu blocks, +%llu invalidations, +%llu flushes, %llu interpreted/frame",
                         run, run - gfx - aud - rsp - jitc, gfx, pr[H64_PROF_RENDER] * k, aud, rsp, jitc, profPresent * k,
                         profWait * k, js ? (unsigned long long)(js->blocksCompiled - jitBlocks) : 0ull,
                         js ? (unsigned long long)(js->invalidations - jitInval) : 0ull,
                         js ? (unsigned long long)(js->flushes - jitFlush) : 0ull,
                         js ? (unsigned long long)((js->helperCalls - jitHelper) / framesSincePerf) : 0ull);
                if (js)
                {
                    u64 ins = sys->cpu.instructions - insnsPrev, idle = js->idleSkipped - jitIdle;
                    H64_INFO("[jit] per frame: %llu instructions executed, %llu skipped idle, %llu blocks run, %llu interpreter steps",
                             (unsigned long long)((ins - idle) / framesSincePerf), (unsigned long long)(idle / framesSincePerf),
                             (unsigned long long)((js->blocksRun - jitRun) / framesSincePerf),
                             (unsigned long long)((js->interpSteps - jitSteps) / framesSincePerf));
                    jitBlocks = js->blocksCompiled; jitInval = js->invalidations; jitFlush = js->flushes; jitHelper = js->helperCalls;
                    jitRun = js->blocksRun; jitIdle = js->idleSkipped; jitSteps = js->interpSteps;
                }
                insnsPrev = sys->cpu.instructions;
                if (xs.bigW)
                    H64_INFO("[xtex] largest decode %ux%u (%u of 64K+ texels) fmt %u size %u stride %u mask %u/%u flags %X from a %s",
                             xs.bigW, xs.bigH, xs.bigCount, xs.bigFmt, xs.bigSize, xs.bigStride, xs.bigMaskS, xs.bigMaskT, xs.bigFlags,
                             xs.bigRect ? "rectangle" : "triangle");
                PcSamplerReport();
                H64_INFO("[cprof] ms/frame inside cpu: interpreter helper %.1f, scheduler events %.1f",
                         pr[H64_PROF_HELPER] * k, pr[H64_PROF_EVENTS] * k);
                H64_INFO("[xprof] ms/frame: rdp commands %.1f (state %.1f, textures %.1f, %u texels/frame) draw calls %.1f",
                         xs.tRdp * k, xs.tState * k, xs.tTexture * k, xs.texelsDecoded / framesSincePerf, xs.tDraw * k);
            }
            memset(sys->prof, 0, sizeof(sys->prof));
            profRun = profPresent = profWait = 0;
            perfStart = GetTickCount();
            framesSincePerf = 0;
            instrAtPerf = sys->cpu.instructions;
        }
    }
    sys->renderer = NULL;
    h64_xenos_free(renderer);
    xb_audio_shutdown();
    s_samplePc = NULL;
    Sleep(5);
    FreeSystem(sys);
    if (s_crashed) MessageScreen(dev, "The game crashed", s_crashText, D3DCOLOR_XRGB(240, 60, 60));
}

int __cdecl main()
{
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    int tests = 0, checks = 0, failures;
    char status[256];
    static char jitReport[512];
    Config cfg;

    s_log = fopen("game:\\harissa64v2.log", "w");
    if (!s_log)
    {
        s_log = fopen("cache:\\harissa64v2.log", "w");
        s_logDrive = "cache";
    }
    h64_log_set_sink(file_sink);
    H64_INFO("[main] Harissa64 V2 %s (Xbox 360), %s-endian, %d-bit pointers", H64_VERSION_STRING,
             H64_HOST_BIG_ENDIAN ? "big" : "little", (int)(sizeof(void *) * 8));
    LoadConfig(&cfg);
    if (cfg.xenia || !cfg.fpuFlags) h64_fenv_disable_host_flags();
    s_jitNoFpu = !cfg.jitFpu;
    s_noRegCache = !cfg.regCache;
    s_cpi = cfg.cpi >= 1 && cfg.cpi <= 8 ? (u32)cfg.cpi : 1;
    H64_INFO("[main] settings: mode=%s cpu=%s hle=%d rom=%s jitfpu=%d cpi=%d regcache=%d", cfg.mode, cfg.cpu, cfg.hle, cfg.rom[0] ? cfg.rom : "(auto)",
             cfg.jitFpu, cfg.cpi, cfg.regCache);

    failures = h64_run_all_unit_tests(&tests, &checks);
    _snprintf(status, sizeof(status), "Unit tests: %s\n%d tests, %d checks, %d failures",
              failures ? "FAILED" : "PASSED", tests, checks, failures);
    status[sizeof(status) - 1] = 0;
    H64_INFO("[main] UNIT %s: %d tests, %d checks, %d failures", failures ? "FAILED" : "PASSED", tests, checks,
             failures);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    {
        H64_ERROR("[main] CreateDevice failed");
        dev = NULL;
    }

    if (failures)
        MessageScreen(dev, "Unit tests failed", status, D3DCOLOR_XRGB(240, 60, 60));
    else if (!strcmp(cfg.mode, "jittest"))
    {
        if (dev)
        {
            dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(14, 16, 26), 1.0f, 0);
            AddText(100, 300, 3, "Recompiler test running...");
            FlushText(dev, D3DCOLOR_XRGB(230, 230, 230));
            dev->Present(NULL, NULL, NULL, NULL);
        }
        RunDynarecTest(&cfg, jitReport, sizeof(jitReport));
        MessageScreen(dev, status, jitReport,
                      strstr(jitReport, "DIVERGED") ? D3DCOLOR_XRGB(240, 60, 60) : D3DCOLOR_XRGB(200, 200, 120));
    }
    else if (dev)
        RunGame(dev, &cfg);

    if (s_log)
        fclose(s_log);
    // Back to the dashboard (Aurora on the console). Xenia has none: it looks
    // for game:\default.xex and shows "Title Launch Failed", so with xenia=1
    // the title just ends.
    if (!cfg.xenia)
        XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    return 0;
}
