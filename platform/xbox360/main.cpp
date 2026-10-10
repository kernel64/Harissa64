// Harissa64 V2 - Xbox 360 entry point.
//
// Start-up: unit tests on the console's own CPU, then, depending on
// game:\harissa64v2.ini (key=value lines, all optional):
//   mode=play      (default) run a ROM: recompiler or interpreter CPU, RSP
//                  task HLE (graphics and audio), Xenos renderer, XAudio2,
//                  XInput. The ROM is rom=<path>, else game:\test.z64, else
//                  the first .z64/.n64/.v64/.zip in game:\roms\.
//   mode=jittest   the M3 recompiler check (lockstep and timings, no video).
//   cpu=dynarec    (default) or cpu=interp (Xenia cannot run generated code).
//   hle=1          (default) or hle=0 for the LLE RSP (slow; no video yet:
//                  the Xenos renderer only draws HLE triangles).
// The log goes to game:\harissa64v2.log, or cache:\ when game:\ is
// read-only (Xenia). Holding BACK for 3 s returns to the ROM list.
#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <string>
#include <ppcintrinsics.h>

#include "../../core/common/h64_types.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../core/common/h64_fenv.h"
#include "../../core/system/h64_system.h"
#include "../../core/dynarec/h64_lockstep.h"
#include "../../core/hle/h64_hle.h"
#include "../../core/pif/h64_input_script.h"
#include "../../core/savestate/h64_state.h"
#include "../../render/xenos/h64_xenos.h"
#include "../../tests/unit/unit_tests.h"
#include "xb_config.h"
#include "xb_ui.h"
#include "xb_menu.h"
#include "xb_audio.h"

// ---- Recompiler code memory (M0.3 result: only an image section linked
// writable + executable runs generated code; /SECTION:.jitc,ERW in the project) ----
extern "C" VOID NTAPI KeSweepIcacheRange(PVOID Address, SIZE_T Size);

// 32 MB (the most a relative branch reaches: hot code in the first half,
// slow paths in the second); 16 MB filled up every ~40 s on DK64 and Conker,
// and each flush recompiled ~6000 blocks.
#define JIT_BYTES (32u * 1024u * 1024u)
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

    // Locality: a chain of 2048 one-instruction blocks ("b next") laid out at
    // different strides through the code cache. The time per hop shows what a
    // jump to code far away costs (instruction cache, L2, address translation).
    {
        static const u32 strides[4] = { 128, 1024, 4096, 8192 };
        char line[200];
        size_t len = 0;
        u32 s;
        line[0] = 0;
        for (s = 0; s < 4; s++)
        {
            u32 stride = strides[s], hops = 2048, words = stride / 4, h, rep;
            BenchFn chain = (BenchFn)(void *)s_jitMem;
            if ((u64)hops * stride + 4096 > JIT_BYTES) hops = (JIT_BYTES - 4096) / stride;
            for (h = 0; h < hops; h++)
            {
                u32 *w = s_jitMem + h * words;
                *w = h + 1 < hops ? 0x48000000u | (stride & 0x03FFFFFCu) : 0x4E800020u;   // b +stride / blr
                FlushIcache(w, 4);
            }
            chain(0);   // warm up
            QueryPerformanceCounter(&a);
            for (rep = 0; rep < 50; rep++) chain(0);
            QueryPerformanceCounter(&b);
            len += _snprintf(line + len, sizeof(line) - len, "%s%u B: %.1f ns", s ? ", " : "", stride,
                             (double)(b.QuadPart - a.QuadPart) * 1e9 / f.QuadPart / (50.0 * hops));
        }
        line[sizeof(line) - 1] = 0;
        H64_INFO("[jit] code locality, per jump between blocks %s", line);
    }
}

static FILE *s_log = NULL;
static const char *s_logDrive = "game";   // where the log (and screenshots) go

// The log goes to memory; a low-priority thread writes it every 500 ms.
// Writing it from the emulation thread stalled it for 100-140 ms every 2-s
// period of [perf] lines (FATX writes), which clock pacing turned into lost
// frames. Warnings and errors are written at once (a crash must not lose them).
static CRITICAL_SECTION s_logLock;
static std::string s_logBuf;
static HANDLE s_logThread;
static volatile LONG s_logQuit;

static CRITICAL_SECTION s_logFileLock;   // the file only: the buffer lock is never held while writing

static void LogFlush(void)
{
    std::string out;
    if (!s_log) return;
    EnterCriticalSection(&s_logFileLock);
    EnterCriticalSection(&s_logLock);
    out.swap(s_logBuf);
    LeaveCriticalSection(&s_logLock);
    if (!out.empty())
    {
        fwrite(out.data(), 1, out.size(), s_log);
        fflush(s_log);
    }
    LeaveCriticalSection(&s_logFileLock);
}

static DWORD WINAPI LogThread(LPVOID)
{
    while (!s_logQuit)
    {
        Sleep(500);
        LogFlush();
    }
    return 0;
}

static void file_sink(int level, const char *line)
{
    // No OutputDebugString: with XBDM loaded (DashLaunch plugin) each call goes
    // to the debug monitor over the network, ~9 ms per line: the [perf] block
    // stalled the emulation 90-120 ms every 2 s.
    if (!s_log) return;
    EnterCriticalSection(&s_logLock);
    s_logBuf += line;
    s_logBuf += '\n';
    LeaveCriticalSection(&s_logLock);
    if (level <= H64_LOG_WARN || !s_logThread) LogFlush();
}

// ---- Configuration: xb_config.cpp ----

static void LoadConfig(Config *c)
{
    ConfigDefaults(c);
    ConfigParseFile(c, "game:\\harissa64v2.ini");
}

// Shows a message until BACK is pressed (and released).
static void MessageScreen(IDirect3DDevice9 *dev, const char *title, const char *text, D3DCOLOR color)
{
    for (;;)
    {
        XINPUT_STATE in;
        if (dev)
        {
            static char wrapped[2048];
            const float x = UI_SAFE_X, y = 140, w = UI_WIDTH - 2 * UI_SAFE_X, h = 470;
            dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
            UiBegin(dev);
            UiBackdrop();
            UiBrand(UI_SAFE_X, 72, 34);
            UiPanel(x, y, w, h, 22);
            UiRoundRect(x + 28, y + 34, 5, 40, 2.5f, color, color);
            UiText(UI_BOLD, 30, x + 52, y + 54, UI_COL_TEXT, title, UI_LEFT);
            UiTextWrap(UI_REGULAR, 21, w - 104, 12, text, wrapped, sizeof(wrapped));
            UiText(UI_REGULAR, 21, x + 52, y + 120, UI_COL_TEXT2, wrapped, UI_LEFT);
            UiScreenFooter("BACK:Continue");
            UiEnd();
            dev->Present(NULL, NULL, NULL, NULL);
        }
        if (XInputGetState(0, &in) == ERROR_SUCCESS && (in.Gamepad.wButtons & XINPUT_GAMEPAD_BACK))
            break;
        Sleep(16);
    }
    for (;;)
    {
        XINPUT_STATE in;
        if (XInputGetState(0, &in) != ERROR_SUCCESS || !(in.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)) break;
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
    static const char *patterns[] = { "game:\\roms\\*.z64", "game:\\roms\\*.n64", "game:\\roms\\*.v64", "game:\\roms\\*.zip" };
    WIN32_FIND_DATAA fd;
    int i;
    if (c->rom[0] && GetFileAttributesA(c->rom) != 0xFFFFFFFF) { strncpy(path, c->rom, len); path[len - 1] = 0; return 1; }
    if (GetFileAttributesA("game:\\test.z64") != 0xFFFFFFFF) { strncpy(path, "game:\\test.z64", len); return 1; }
    for (i = 0; i < 4; i++)
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

// ---- Workers: jobs run in order on other hardware threads while the CPU
// thread emulates.
//  - Graphics (asyncgfx=1, hardware thread 2): graphics HLE tasks and frame
//    presents. The D3D device belongs to one thread at a time
//    (AcquireThreadOwnership): the worker takes it for each job; the CPU
//    thread takes it back only after waiting for every queued job, and gives
//    it away before queuing one.
//  - Audio (asyncaudio=1, hardware thread 4): audio HLE tasks.
#define WORK_SLOTS 8
struct WorkJob { void (*fn)(void *); void *arg; };
struct WorkQueue
{
    WorkJob jobs[WORK_SLOTS];
    volatile LONG queued, done;
    HANDLE sem, doneEvt, thread;
    volatile LONG quit;
    int usesDevice;   // the graphics queue: D3D ownership changes hands
};
static WorkQueue s_gfxQ, s_audioQ;
static IDirect3DDevice9 *s_workDev;
static H64Renderer *s_workRenderer;
static int s_ownsDevice = 1;
static u32 s_presentRegs[WORK_SLOTS][32];

static DWORD WINAPI WorkThread(LPVOID param)
{
    WorkQueue *q = (WorkQueue *)param;
    for (;;)
    {
        WaitForSingleObject(q->sem, INFINITE);
        if (q->quit) break;
        {
            WorkJob *j = &q->jobs[q->done % WORK_SLOTS];
            if (q->usesDevice) s_workDev->AcquireThreadOwnership();
            j->fn(j->arg);
            if (q->usesDevice) s_workDev->ReleaseThreadOwnership();
        }
        InterlockedIncrement(&q->done);
        SetEvent(q->doneEvt);
    }
    return 0;
}

static void QueueWait(WorkQueue *q)
{
    if (!q->thread) return;
    while (q->done != q->queued) WaitForSingleObject(q->doneEvt, INFINITE);
}

// Until job `ticket` (a QueuePush result) is done.
static void QueueWaitTicket(WorkQueue *q, u32 ticket)
{
    if (!q->thread) return;
    while ((s32)((u32)q->done - ticket) < 0) WaitForSingleObject(q->doneEvt, INFINITE);
}

static u32 QueuePush(WorkQueue *q, void (*fn)(void *), void *arg)
{
    if (q->queued - q->done >= WORK_SLOTS - 1) QueueWaitTicket(q, (u32)q->done + 1);
    q->jobs[q->queued % WORK_SLOTS].fn = fn;
    q->jobs[q->queued % WORK_SLOTS].arg = arg;
    InterlockedIncrement(&q->queued);
    ReleaseSemaphore(q->sem, 1, NULL);
    return (u32)q->queued;
}

static int QueueStart(WorkQueue *q, int usesDevice, DWORD hwThread)
{
    q->queued = q->done = 0;
    q->quit = 0;
    q->usesDevice = usesDevice;
    q->sem = CreateSemaphore(NULL, 0, 1000, NULL);
    q->doneEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!q->sem || !q->doneEvt) return -1;
    q->thread = CreateThread(NULL, 256 * 1024, WorkThread, q, CREATE_SUSPENDED, NULL);
    if (!q->thread) return -1;
    XSetThreadProcessor(q->thread, hwThread);
    ResumeThread(q->thread);
    return 0;
}

static void QueueStop(WorkQueue *q)
{
    if (!q->thread) return;
    QueueWait(q);
    q->quit = 1;
    ReleaseSemaphore(q->sem, 1, NULL);
    WaitForSingleObject(q->thread, INFINITE);
    CloseHandle(q->thread);
    CloseHandle(q->sem);
    CloseHandle(q->doneEvt);
    q->thread = NULL;
}

// Graphics queue: everything queued is done and the device is the CPU thread's again.
static void WorkWaitAll(void *)
{
    QueueWait(&s_gfxQ);
    if (!s_ownsDevice)
    {
        s_workDev->AcquireThreadOwnership();
        s_ownsDevice = 1;
        h64_xenos_set_worker(s_workRenderer, 0);   // RDRAM copy-backs reported to the recompiler now
    }
}

static u32 WorkStart(void *, void (*fn)(void *), void *arg)
{
    if (s_ownsDevice)
    {
        QueueWait(&s_gfxQ);
        h64_xenos_set_worker(s_workRenderer, 1);
        s_workDev->ReleaseThreadOwnership();
        s_ownsDevice = 0;
    }
    return QueuePush(&s_gfxQ, fn, arg);
}

static void WorkWaitTicket(void *, u32 ticket) { QueueWaitTicket(&s_gfxQ, ticket); }

static void AudioStart(void *, void (*fn)(void *), void *arg) { QueuePush(&s_audioQ, fn, arg); }
static void AudioWait(void *) { QueueWait(&s_audioQ); }

// Present pacing statistics: intervals between the moments frames reach the
// screen (QPC), for the [pace] line. Written by the thread that presents.
static LARGE_INTEGER s_presLast;
static double s_presSum, s_presSumSq, s_presMax, s_presMin = 1e9;
static u32 s_presCount, s_presLong;
// Longest time per loop section in the period (ms), to find stalls.
static double s_maxRun, s_maxPresent, s_maxSaves, s_maxWait, s_lastPerfBlock;

static void NotePresent(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (s_presLast.QuadPart)
    {
        double ms = (double)(now.QuadPart - s_presLast.QuadPart) * 1000.0 / (double)freq.QuadPart;
        s_presSum += ms;
        s_presSumSq += ms * ms;
        if (ms > s_presMax) s_presMax = ms;
        if (ms < s_presMin) s_presMin = ms;
        if (ms > 25.0) s_presLong++;
        s_presCount++;
    }
    s_presLast = now;
}

static void PresentJob(void *arg)
{
    h64_xenos_present_vi(s_workRenderer, (const u32 *)arg);
    NotePresent();
}

// The frame's present: on the graphics worker when it runs, with the VI registers of now.
static void PresentFrame(H64System *sys, H64Renderer *renderer)
{
    if (s_gfxQ.thread)
    {
        u32 *regs = s_presentRegs[s_gfxQ.queued % WORK_SLOTS];
        memcpy(regs, sys->vi.regs, sizeof(s_presentRegs[0]) < sizeof(sys->vi.regs) ? sizeof(s_presentRegs[0]) : sizeof(sys->vi.regs));
        WorkStart(NULL, PresentJob, regs);
    }
    else
    {
        h64_xenos_present(renderer);
        NotePresent();
    }
}

static void WorkersStart(H64System *sys, IDirect3DDevice9 *dev, H64Renderer *renderer, int gfx, u32 gfxCycles, int audio,
                         u32 audioCycles)
{
    s_workDev = dev;
    s_workRenderer = renderer;
    s_ownsDevice = 1;
    sys->asyncUser = NULL;
    if (gfx)
    {
        if (QueueStart(&s_gfxQ, 1, 2) == 0)
        {
            sys->asyncStart = WorkStart;
            sys->asyncWait = WorkWaitAll;
            sys->asyncWaitTicket = WorkWaitTicket;
            sys->asyncGfxCycles = gfxCycles;
            H64_INFO("[main] graphics worker on hardware thread 2 (graphics tasks keep the RSP busy %u cycles)", gfxCycles);
        }
        else
            H64_WARN("[main] graphics worker could not start: graphics on the CPU thread");
    }
    if (audio)
    {
        if (QueueStart(&s_audioQ, 0, 4) == 0)
        {
            sys->asyncAudioStart = AudioStart;
            sys->asyncAudioWait = AudioWait;
            sys->asyncAudioCycles = audioCycles;
            H64_INFO("[main] audio worker on hardware thread 4 (audio tasks keep the RSP busy %u cycles)", audioCycles);
        }
        else
            H64_WARN("[main] audio worker could not start: audio HLE on the CPU thread");
    }
}

static void WorkersStop(H64System *sys)
{
    QueueStop(&s_audioQ);
    if (s_gfxQ.thread)
    {
        WorkWaitAll(NULL);
        QueueStop(&s_gfxQ);
    }
    sys->asyncStart = NULL;
    sys->asyncWait = NULL;
    sys->asyncWaitTicket = NULL;
    sys->asyncAudioStart = NULL;
    sys->asyncAudioWait = NULL;
}

static int s_jitNoFpu;   // jitfpu=0
static int s_noRegCache; // regcache=0
static int s_noFpCache;  // fpcache=0
static int s_fastFpu;    // fastfpu=1
static int s_fullExits;  // fullexits=1
static int s_noSuper;    // superblocks=0

static u32 s_cpi = 2;    // cpi=N

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
    if (sys->jit) sys->jit->noFpCache = s_noFpCache;
    if (sys->jit && s_fastFpu) { sys->jit->fastFpu = 1; h64_jit_reset(sys); }
    if (sys->jit && s_fullExits) { sys->jit->fullExits = 1; h64_jit_reset(sys); }
    if (sys->jit && s_noSuper) { sys->jit->noSuper = 1; h64_jit_reset(sys); }
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
    data = RomFileLoad(rom, &size);
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

// The menu's Graphics settings on the renderer.
static void ApplyGraphics(H64Renderer *renderer, const Config *c)
{
    H64XenosOptions o;
    if (!renderer) return;
    o.scale = c->resScale;
    o.texFilter = c->texFilter;
    o.smooth = c->smooth;
    o.sharpen = c->sharpen;
    o.blur = c->blur;
    o.screen = c->screen;
    o.aspect = c->aspect;
    h64_xenos_set_options(renderer, &o);
}

// ---- mode=play ----
static XINPUT_STATE s_pad;                  // controller 1 (also the front end's shortcuts)
static int s_padValid;
static XINPUT_STATE s_padsMore[3];          // controllers 2-4: N64 ports 2-4
static int s_padsMoreValid[3];
static const H64InputScript *s_script;   // scripted input (input=), NULL: the controller

static s8 StickAxis(SHORT v)
{
    int x = v;
    const int dead = 7000;
    if (x > -dead && x < dead) return 0;
    x = x > 0 ? (x - dead) * 80 / (32767 - dead) : (x + dead) * 80 / (32768 - dead);
    return (s8)(x > 80 ? 80 : x < -80 ? -80 : x);
}

// An Xbox 360 controller as an N64 one (mapping as V1).
static void PadFromXInput(const XINPUT_GAMEPAD *g, H64Pad *pad)
{
    u16 b = 0;
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
    pad->buttons = b;
    pad->x = StickAxis(g->sThumbLX);
    pad->y = StickAxis(g->sThumbLY);
}

// Called by the PIF just before the game reads the controllers. Controller n
// (XInput user n) is N64 port n + 1; port 1 is always plugged in, ports 2-4
// while their controller is connected.
static void PadHook(H64System *sys)
{
    int i;
    if (s_script) { h64_input_script_apply(s_script, sys); return; }
    sys->padMask = 1;
    // BACK held: the buttons are front-end shortcuts, not the game's.
    if (!s_padValid || (s_pad.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)) { sys->pad[0].buttons = 0; sys->pad[0].x = sys->pad[0].y = 0; }
    else PadFromXInput(&s_pad.Gamepad, &sys->pad[0]);
    for (i = 0; i < 3; i++)
    {
        if (s_padsMoreValid[i])
        {
            sys->padMask |= 2u << i;
            PadFromXInput(&s_padsMore[i].Gamepad, &sys->pad[i + 1]);
        }
        else
        {
            sys->pad[i + 1].buttons = 0;
            sys->pad[i + 1].x = sys->pad[i + 1].y = 0;
        }
    }
}

// The profiling clock: the time base register (49.875 MHz; the [prof] lines
// divide by the performance counter's 50 MHz, 0.25 % off). Read thousands of
// times a frame (each vertex batch); QueryPerformanceCounter cost enough to
// show in what it measured.
static u64 ProfClock(void)
{
    return __mftb();
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

// ---- Game saves ----
// One folder per game: <drive>:\saves\<name> <game code>\ with eeprom.bin,
// sram.bin, flash.bin and pak1.bin (the drive of the log: game:, or cache:
// in Xenia). Written 2 s after the game first changes them, and on leaving.
static char s_saveDir[128];
static u32 s_saveDirtyAt;

static void SavesOpen(H64System *sys)
{
    char folder[64];
    h64_save_folder_name(&sys->rom, folder, sizeof(folder));
    _snprintf(s_saveDir, sizeof(s_saveDir), "%s:\\saves", s_logDrive);
    s_saveDir[sizeof(s_saveDir) - 1] = 0;
    CreateDirectory(s_saveDir, NULL);
    _snprintf(s_saveDir, sizeof(s_saveDir), "%s:\\saves\\%s", s_logDrive, folder);
    s_saveDir[sizeof(s_saveDir) - 2] = 0;
    CreateDirectory(s_saveDir, NULL);
    strcat(s_saveDir, "\\");
    s_saveDirtyAt = 0;
    H64_INFO("[save] folder %s: %d file(s) loaded", s_saveDir, h64_save_load(sys->save, s_saveDir));
}

static void SavesTick(H64System *sys, u32 presented, int leaving)
{
    if (!sys->save->dirty) { s_saveDirtyAt = 0; return; }
    if (!s_saveDirtyAt) s_saveDirtyAt = presented ? presented : 1;
    if (leaving || presented - s_saveDirtyAt >= 120)
    {
        h64_save_store(sys->save, s_saveDir);
        s_saveDirtyAt = 0;
    }
}

// ---- Save states ----
// <save folder>\state<N>.h64s. While BACK is held: RB saves, LB loads,
// D-pad left/right change the slot. A message shows the result for 2 s.
static int s_stateSlot = 1;
static int s_statePending;          // a save waits for a quiet point (no HLE task in progress)
static char s_osd[64];
static DWORD s_osdUntil;
#define OSD_MS 2000                 // how long a toast stays

static void Osd(const char *text)
{
    strncpy(s_osd, text, sizeof(s_osd) - 1);
    s_osd[sizeof(s_osd) - 1] = 0;
    s_osdUntil = GetTickCount() + OSD_MS;
    H64_INFO("[main] %s", text);
}

static int s_showFps;               // showfps=1: frames shown per second in the top-right corner
static char s_fpsText[16];
static UiMenu s_menu;
static int s_menuOpen;              // the in-game menu is drawn over the frozen frame
static int s_aboutOpen;             // ... showing the About panel

// Drawn by the renderer just before each present (on the graphics worker when it runs).
static void OverlayHook(void *, IDirect3DDevice9 *dev)
{
    // Nothing to show (the usual case in play): no state change at all.
    s32 left = (s32)(s_osdUntil - GetTickCount());
    int fps = s_showFps && s_fpsText[0], osd = s_osd[0] && left >= 0;
    if (!fps && !osd && !s_menuOpen) return;
    UiBegin(dev);
    if (fps) UiFpsBadge(s_fpsText);
    if (s_menuOpen && s_aboutOpen) AboutDraw();
    else if (s_menuOpen) UiMenuDraw(&s_menu);
    if (osd) UiToast(s_osd, (float)(OSD_MS - left), (float)OSD_MS);
    UiEnd();
}

static void StatePath(char *out, size_t size)
{
    _snprintf(out, size, "%sstate%d.h64s", s_saveDir, s_stateSlot);
    out[size - 1] = 0;
}

// Returns 1 when done (saved or failed), 0 when the machine is not quiet yet.
static int SaveStateNow(H64System *sys)
{
    std::vector<u8> st;
    char path[160], msg[64];
    FILE *f;
    if (!h64_state_quiet(sys)) return 0;
    h64_state_save(sys, &st);
    StatePath(path, sizeof(path));
    f = fopen(path, "wb");
    if (f && fwrite(&st[0], 1, st.size(), f) == st.size())
        _snprintf(msg, sizeof(msg), "State %d saved", s_stateSlot);
    else
        _snprintf(msg, sizeof(msg), "State %d: write failed", s_stateSlot);
    if (f) fclose(f);
    msg[sizeof(msg) - 1] = 0;
    Osd(msg);
    return 1;
}

static void LoadStateNow(H64System *sys)
{
    char path[160], msg[64];
    u32 size = 0;
    u8 *data;
    StatePath(path, sizeof(path));
    data = LoadFile(path, &size);
    if (!data)
        _snprintf(msg, sizeof(msg), "No state in slot %d", s_stateSlot);
    else if (h64_state_load(sys, data, size))
        _snprintf(msg, sizeof(msg), "State %d: cannot load it", s_stateSlot);
    else
        _snprintf(msg, sizeof(msg), "State %d loaded", s_stateSlot);
    free(data);
    msg[sizeof(msg) - 1] = 0;
    Osd(msg);
}

// ---- In-game menu ----
// BACK pressed and released (without RB/LB/D-pad) pauses the game and shows
// it over the frozen frame. Settings changed there apply at once (audio
// margin, smoothing, FPS) or at the next start (CPU, RSP); they are saved
// for all games (config\settings.ini) or for this game (config\<game>.ini).
enum { RG_DASHBOARD = 0, RG_BROWSER, RG_CONTINUE };
enum { M_RESUME = 0, M_SAVE, M_LOAD, M_SLOT, M_SETTINGS, M_RESET, M_ROMS, M_DASHBOARD };   // X opens About
static char s_settingsPath[160];    // <drive>:\config\settings.ini ("" when the menu files are not used)
static char s_profilePath[160];     // <drive>:\config\<game>.ini

static void BuildGameMenu(void)
{
    char slot[16];
    UiMenuClear(&s_menu, "Paused", "A:Select|DPAD:Change|X:About|B/START:Resume");
    UiMenuAdd(&s_menu, "Resume", "");
    UiMenuAdd(&s_menu, "Save state", "");
    UiMenuAdd(&s_menu, "Load state", "");
    sprintf(slot, "< %d >", s_stateSlot);
    UiMenuAdd(&s_menu, "State slot", slot);
    UiMenuAdd(&s_menu, "Settings", "");
    UiMenuAdd(&s_menu, "Reset the game", "");
    UiMenuAdd(&s_menu, "Back to the ROM list", "");
    UiMenuAdd(&s_menu, "Quit to the dashboard", "");
}

static void ResetGame(H64System *sys)
{
    h64_hle_async_wait(sys);
    if (sys->asyncAudioWait) sys->asyncAudioWait(sys->asyncUser);
    h64_system_reset(sys);
    if (sys->renderer && sys->renderer->reset) sys->renderer->reset(sys->renderer->user);
    Osd("Game reset");
}

// Runs the menu; returns RG_CONTINUE, RG_BROWSER or RG_DASHBOARD.
static int GameMenu(H64System *sys, H64Renderer *renderer, Config *c)
{
    UiInput in;
    int settings = 0, sel, result = -1;
    if (s_gfxQ.thread) WorkWaitAll(NULL);   // the device is this thread's while the menu shows
    BuildGameMenu();
    UiInputInit(&in);
    s_menuOpen = 1;
    while (result < 0)
    {
        WORD down = UiInputPoll(&in);
        if (s_aboutOpen)
        {
            if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_X)) s_aboutOpen = 0;
        }
        else if (settings)
        {
            int r = SettingsInput(&s_menu, c, s_settingsPath[0] != 0, down);
            if (r == SET_CHANGED)
            {
                xb_audio_set_target_ms(c->audioMs);
                ApplyGraphics(renderer, c);
                s_showFps = c->showFps;
            }
            else if (r == SET_SAVE_ALL || r == SET_SAVE_GAME)
            {
                const char *path = r == SET_SAVE_ALL ? s_settingsPath : s_profilePath;
                if (!path[0]) Osd("Not saved: started by rom= or a script");
                else Osd(ConfigWriteMenuKeys(c, path, r == SET_SAVE_ALL) ? (r == SET_SAVE_ALL ? "Saved for all games" : "Saved for this game")
                                                                         : "Settings: write failed");
            }
            if (r == SET_BACK || r == SET_SAVE_ALL || r == SET_SAVE_GAME)
            {
                settings = 0;
                BuildGameMenu();
                s_menu.sel = M_SETTINGS;
            }
        }
        else
        {
            UiMenuNavigate(&s_menu, down);
            sel = s_menu.sel;
            if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) result = RG_CONTINUE;
            else if (down & XINPUT_GAMEPAD_X) s_aboutOpen = 1;   // the About panel (X, as in the ROM browser)
            if (sel == M_SLOT && (down & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A)))
            {
                s_stateSlot += (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 1;
                if (s_stateSlot < 1) s_stateSlot = 9;
                if (s_stateSlot > 9) s_stateSlot = 1;
                sprintf(s_menu.value[M_SLOT], "< %d >", s_stateSlot);
            }
            else if (down & XINPUT_GAMEPAD_A)
            {
                switch (sel)
                {
                case M_RESUME: result = RG_CONTINUE; break;
                case M_SAVE: s_statePending = 1; result = RG_CONTINUE; break;   // at the next quiet point
                case M_LOAD: LoadStateNow(sys); result = RG_CONTINUE; break;
                case M_SETTINGS: settings = 1; SettingsBuild(&s_menu, c, s_profilePath[0] != 0); break;
                case M_RESET: ResetGame(sys); result = RG_CONTINUE; break;
                case M_ROMS: result = RG_BROWSER; break;
                case M_DASHBOARD: result = RG_DASHBOARD; break;
                }
            }
        }
        h64_xenos_present(renderer);   // the frozen frame, the menu (overlay) on top
        Sleep(16);
    }
    s_menuOpen = 0;
    s_aboutOpen = 0;
    return result;
}

// Real time in ms from QueryPerformanceCounter. While the emulator runs, the
// console's system clock and GetTickCount fall behind real time (measured
// 2026-10-08 against the PC through XBDM: the system clock counted 96.2 s in
// 100 s, GetTickCount ~1.4 % less again); QPC stays within 0.3 %.
static double QpcMs(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// The menu with a crash guard: an exception there is logged (and the log
// flushed) instead of DashLaunch's crash notice and a silent return to Aurora.
static int GameMenuSafe(H64System *sys, H64Renderer *renderer, Config *c)
{
    int r = RG_DASHBOARD;
    H64_INFO("[menu] opened at VI %u", sys->vi.frames);
    LogFlush();
    __try
    {
        r = GameMenu(sys, renderer, c);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s_menuOpen = 0;
        H64_ERROR("[crash] exception %08X in the in-game menu", GetExceptionCode());
        r = RG_BROWSER;
    }
    H64_INFO("[menu] closed: %s", r == RG_CONTINUE ? "resume" : r == RG_BROWSER ? "ROM list" : "dashboard");
    LogFlush();
    return r;
}

static int RunGame(IDirect3DDevice9 *dev, Config *c, const char *romPath)
{
    int result = RG_DASHBOARD, menuRequest = 0;
    u32 fpsFrames = 0;
    double fpsStart = QpcMs();
    char rom[256];
    u8 *data;
    u32 size = 0;
    int jit = !strcmp(c->cpu, "dynarec");
    H64System *sys;
    H64Renderer *renderer = NULL;
    LARGE_INTEGER freq, now, frameStart;
    double perfStart = QpcMs();
    u32 framesSincePerf = 0, presented = 0;
    LARGE_INTEGER t0, t1, t2;
    LONGLONG profRun = 0, profPresent = 0, profWait = 0;
    u64 jitBlocks = 0, jitInval = 0, jitFlush = 0, jitHelper = 0, jitRun = 0, jitIdle = 0, jitSteps = 0, insnsPrev = 0;
    u64 instrAtPerf = 0;

    s_crashed = 0;
    s_showFps = c->showFps;
    s_fpsText[0] = 0;
    if (romPath)
    {
        strncpy(rom, romPath, sizeof(rom) - 1);
        rom[sizeof(rom) - 1] = 0;
    }
    else if (!FindRom(c, rom, sizeof(rom)))
    {
        MessageScreen(dev, "No ROM found", "Put a ROM at game:\\test.z64 or in game:\\roms\\", D3DCOLOR_XRGB(240, 200, 60));
        return RG_DASHBOARD;
    }
    data = RomFileLoad(rom, &size);
    if (!data) { MessageScreen(dev, "Cannot read the ROM", rom, D3DCOLOR_XRGB(240, 60, 60)); return RG_BROWSER; }
    sys = MakeSystem(data, size, jit);
    free(data);
    if (!sys) { MessageScreen(dev, "Not an N64 ROM", rom, D3DCOLOR_XRGB(240, 60, 60)); return RG_BROWSER; }
    sys->options.hleGfx = c->hle;
    sys->options.hleAudio = c->hle;
    sys->padHook = PadHook;
    sys->profClock = ProfClock;
    s_script = c->input.count ? &c->input : NULL;
    xb_audio_set_target_ms(c->audioMs);
    H64_INFO("[main] audio queue target %d ms", xb_audio_target_ms());
    if (xb_audio_init() == 0)
    {
        sys->aiSink = xb_audio_sink;
        sys->aiUser = NULL;
    }
    renderer = h64_xenos_create(sys, dev);
    if (!renderer) { MessageScreen(dev, "Renderer initialisation failed", "See harissa64v2.log", D3DCOLOR_XRGB(240, 60, 60)); FreeSystem(sys); return RG_DASHBOARD; }
    if (c->xenosDebug) h64_xenos_set_debug(renderer, c->xenosDebug);
    ApplyGraphics(renderer, c);
    if (c->softRenderer) sys->options.rdpStateOnly = 0;
    else sys->renderer = renderer;
    H64_INFO("[main] running %s: cpu %s, RSP %s, %s renderer", rom, jit ? "recompiler" : "interpreter",
             c->hle ? "HLE" : "LLE", c->softRenderer ? "software" : "Xenos");
    if (c->hle && !c->trace)
        WorkersStart(sys, dev, renderer, c->asyncGfx && !c->softRenderer, c->gfxCycles ? c->gfxCycles : 400000,
                     c->asyncAudio, c->audioCycles ? c->audioCycles : 100000);

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
        return RG_DASHBOARD;
    }
    SavesOpen(sys);
    s_stateSlot = c->stateSlot >= 1 && c->stateSlot <= 9 ? c->stateSlot : 1;
    s_statePending = 0;
    s_osd[0] = 0;
    h64_xenos_set_overlay(renderer, OverlayHook, NULL);
    if (c->loadState) LoadStateNow(sys);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&frameStart);
    if (!c->xenia)
    {
        BenchCodeMemory();
        // The benchmark wrote over the code memory, where the recompiler keeps its
        // shared entry/exit code: rebuild it.
        if (sys->jit) h64_jit_reset(sys);
    }
    PcSamplerStart(sys);
    DWORD backSince = 0;
    int backUsed = 0;
    double backHeldMs = 0, backPolledAt = 0;   // time BACK was seen held, poll after poll
    int backHinted = 0;
    WORD prevButtons = 0;
    for (;;)
    {
        int queued;
        if (XInputGetState(0, &s_pad) == ERROR_SUCCESS) s_padValid = 1;
        else s_padValid = 0;
        {
            int k;
            for (k = 0; k < 3; k++) s_padsMoreValid[k] = XInputGetState(k + 1, &s_padsMore[k]) == ERROR_SUCCESS;
            // The plugged ports, for the games' controller probes (osContInit).
            sys->padMask = 1;
            if (!s_script)
                for (k = 0; k < 3; k++) if (s_padsMoreValid[k]) sys->padMask |= 2u << k;
        }
        // BACK held for 3 s goes back to the ROM list (the dashboard is left
        // only from the in-game menu or the ROM list); with BACK held, RB/LB
        // save/load a state and the D-pad changes the slot. The hold time adds
        // up the gaps between polls that saw BACK down, each counted 100 ms at
        // most: a slow first frame (a 6105 game's ~4 s boot) between two
        // presses no longer passes for a long hold (it sent the user back to
        // Aurora).
        if (s_padValid && (s_pad.Gamepad.wButtons & XINPUT_GAMEPAD_BACK))
        {
            WORD down = s_pad.Gamepad.wButtons & ~prevButtons;
            double nowMs = QpcMs();
            if (!backSince)
            {
                backSince = GetTickCount() | 1;
                backUsed = 0;
                backHeldMs = 0;
                backHinted = 0;
                H64_INFO("[pad] BACK down at VI %u", presented);
                LogFlush();
            }
            else
                backHeldMs += nowMs - backPolledAt > 100.0 ? 100.0 : nowMs - backPolledAt;
            backPolledAt = nowMs;
            if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) { s_statePending = 1; backUsed = 1; }
            if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) { LoadStateNow(sys); backUsed = 1; }
            if (down & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT))
            {
                char msg[32];
                s_stateSlot += (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : -1;
                if (s_stateSlot < 1) s_stateSlot = 9;
                if (s_stateSlot > 9) s_stateSlot = 1;
                _snprintf(msg, sizeof(msg), "Slot %d", s_stateSlot);
                msg[sizeof(msg) - 1] = 0;
                Osd(msg);
                backUsed = 1;
            }
            if (!backUsed && backHeldMs >= 1000.0 && !backHinted)
            {
                Osd("Keep holding BACK: ROM list");
                backHinted = 1;
            }
            if (!backUsed && backHeldMs >= 3000.0)
            {
                H64_INFO("[pad] BACK held 3 s: back to the ROM list");
                result = RG_BROWSER;
                break;
            }
        }
        else
        {
            if (backSince)
            {
                H64_INFO("[pad] BACK up after %u ms%s", GetTickCount() - backSince, backUsed ? " (used with RB/LB/D-pad)" : "");
                LogFlush();
            }
            if (backSince && !backUsed && backHeldMs < 600.0) menuRequest = 1;   // a short press
            backSince = 0;
        }
        prevButtons = s_padValid ? s_pad.Gamepad.wButtons : 0;
        if (c->saveStateAt && presented == c->saveStateAt) s_statePending = 1;
        if (c->menuAt && presented == c->menuAt) menuRequest = 1;
        if (c->aboutAt && presented == c->aboutAt) { menuRequest = 1; s_aboutOpen = 1; }   // remote check of the About panel
        if (c->browserAt && presented == c->browserAt)
        {
            H64_INFO("[main] browserat reached: back to the ROM list");
            result = RG_BROWSER;
            break;
        }
        if (s_statePending && SaveStateNow(sys)) s_statePending = 0;
        if (menuRequest)
        {
            menuRequest = 0;
            result = GameMenuSafe(sys, renderer, c);
            if (result != RG_CONTINUE) break;
            result = RG_DASHBOARD;
            QueryPerformanceCounter(&frameStart);   // no catching up for the paused time
            prevButtons = 0;
            continue;
        }
        QueryPerformanceCounter(&t0);
        if (RunFrame(sys)) { result = RG_BROWSER; break; }
        QueryPerformanceCounter(&t1);
        PresentFrame(sys, renderer);
        QueryPerformanceCounter(&t2);
        profRun += t1.QuadPart - t0.QuadPart;
        profPresent += t2.QuadPart - t1.QuadPart;
        {
            double r = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart, pr = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / freq.QuadPart;
            if (r > s_maxRun) s_maxRun = r;
            if (pr > s_maxPresent) s_maxPresent = pr;
        }
        framesSincePerf++;
        presented++;
        fpsFrames++;
        if (QpcMs() - fpsStart >= 1000.0)
        {
            double nowMs = QpcMs();
            sprintf(s_fpsText, "%.1f FPS", fpsFrames * 1000.0 / (nowMs - fpsStart));
            fpsFrames = 0;
            fpsStart = nowMs;
        }
        {
            double s0 = QpcMs();
            SavesTick(sys, presented, 0);
            if (QpcMs() - s0 > s_maxSaves) s_maxSaves = QpcMs() - s0;
        }
        {
            int i;
            for (i = 0; i < c->shotCount; i++)
                if (c->shots[i] == presented)
                {
                    char path[64];
                    _snprintf(path, sizeof(path), "%s:\\shot_%05u.bmp", s_logDrive, presented);
                    path[sizeof(path) - 1] = 0;
                    h64_hle_async_wait(sys);
                    WorkWaitAll(NULL);
                    H64_INFO("[main] screenshot %s: %s", path, h64_xenos_save_frame(renderer, path) ? "failed" : "saved");
                }
        }
        if (c->pauseAt && presented == c->pauseAt)
        {
            int k;
            H64_INFO("[main] paused at VI %u", presented);
            if (s_gfxQ.thread) WorkWaitAll(NULL);
            for (k = 0; k < 400; k++) { Sleep(50); h64_xenos_present(renderer); }
            H64_INFO("[main] resumed");
        }
        if (c->exitAfter && presented >= c->exitAfter)
        {
            H64_INFO("[main] exitafter reached (%u VIs)", presented);
            break;
        }

        // Pacing on the clock, one VI period per frame, so frames reach the
        // screen evenly (pacing on the audio queue alone ran several frames
        // back to back, then waited: Paper Mario's name screen, intervals
        // from 2 to 130 ms at 60 FPS on average). XAudio2's rate control keeps
        // the queue near its target; the queue only nudges the period by up
        // to 1 %, and stops the clock if it ran far over the target.
        QueryPerformanceCounter(&t0);
        queued = xb_audio_queued_ms();
        {
            LONGLONG frameTicks = freq.QuadPart * sys->vi.frameCycles / 93750000;
            int target = xb_audio_target_ms();
            if (queued >= 0 && target > 0)
            {
                double e = (double)(queued - target) / target;   // > 0: too much sound queued, slow down
                if (e > 1.0) e = 1.0;
                if (e < -1.0) e = -1.0;
                frameTicks += (LONGLONG)(frameTicks * 0.01 * e);
            }
            for (;;)
            {
                LONGLONG left;
                QueryPerformanceCounter(&now);
                left = frameTicks - (now.QuadPart - frameStart.QuadPart);
                if (left <= 0) break;
                if (left > freq.QuadPart / 500) Sleep(1);   // more than 2 ms left: sleep, then spin to the deadline
            }
            frameStart.QuadPart += frameTicks;
            if (now.QuadPart - frameStart.QuadPart > frameTicks * 4) frameStart = now;   // far behind: do not catch up
            if (queued > 2 * target && target > 0)
            {
                // Far too much sound queued (after a pause, a load): let it drain.
                while (xb_audio_queued_ms() > target) Sleep(1);
                QueryPerformanceCounter(&frameStart);
            }
        }

        QueryPerformanceCounter(&t1);
        profWait += t1.QuadPart - t0.QuadPart;
        {
            double wms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
            if (wms > s_maxWait) s_maxWait = wms;
        }
        if (QpcMs() - perfStart >= 2000.0)
        {
            double perfNow = QpcMs(), ms = perfNow - perfStart;
            H64XenosStats xs;
            XbAudioStats as;
            h64_xenos_stats(renderer, &xs, 1);
            xb_audio_stats(&as, 1);
            H64_INFO("[perf] vi/s=%.1f mips=%.1f tris=%u rects=%u fills=%u texup=%u (new %u) shaders=%u copyback=%u fbswitch=%u audio=%u buffers %u underruns fill %u ms rate %u.%03u played %u Hz submitted %u Hz dropped %u",
                     framesSincePerf * 1000.0 / ms, (double)(sys->cpu.instructions - instrAtPerf) / (ms * 1000.0),
                     xs.triangles, xs.rects, xs.fills, xs.textureUploads, xs.textureCreates, xs.shaderCompiles, xs.copyBacks, xs.fbSwitches, as.buffers, as.underruns, as.fillMs,
                     as.ratioPermille / 1000, as.ratioPermille % 1000, as.playedHz, as.submittedHz, as.dropped);
            if (s_presCount)
            {
                double mean = s_presSum / s_presCount, var = s_presSumSq / s_presCount - mean * mean;
                H64_INFO("[pace] %u presents: interval mean %.1f ms, sd %.1f, min %.1f, max %.1f, %u over 25 ms | longest: run %.1f, present %.1f, saves %.1f, wait %.1f, last stats block %.1f ms",
                         s_presCount, mean, var > 0 ? sqrt(var) : 0.0, s_presMin, s_presMax, s_presLong, s_maxRun, s_maxPresent, s_maxSaves,
                         s_maxWait, s_lastPerfBlock);
                s_maxRun = s_maxPresent = s_maxSaves = s_maxWait = 0;
                s_presSum = s_presSumSq = s_presMax = 0;
                s_presMin = 1e9;
                s_presCount = s_presLong = 0;
            }
            {
                // Audio the game queued in this period, and what it read back from AI_LEN.
                static u64 q0, r0, rs0;
                static u32 b0;
                u32 nb = sys->ai.statBuffers - b0;
                u64 nr = sys->ai.statReads - r0;
                H64_INFO("[ai] %u buffers, %.1f samples each; %llu AI_LEN reads, %.1f samples left on average", nb,
                         nb ? (sys->ai.statQueued - q0) / 4.0 / nb : 0.0, (unsigned long long)nr,
                         nr ? (sys->ai.statReadSum - rs0) / 4.0 / nr : 0.0);
                q0 = sys->ai.statQueued; r0 = sys->ai.statReads; rs0 = sys->ai.statReadSum; b0 = sys->ai.statBuffers;
            }
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
                         run, run - (s_gfxQ.thread ? 0.0 : gfx) - (s_audioQ.thread ? 0.0 : aud) - ((s_gfxQ.thread || s_audioQ.thread) ? pr[H64_PROF_ASYNC_WAIT] * k : 0.0) - rsp - jitc, gfx,
                         pr[H64_PROF_RENDER] * k, aud, rsp, jitc, profPresent * k,
                         profWait * k, js ? (unsigned long long)(js->blocksCompiled - jitBlocks) : 0ull,
                         js ? (unsigned long long)(js->invalidations - jitInval) : 0ull,
                         js ? (unsigned long long)(js->flushes - jitFlush) : 0ull,
                         js ? (unsigned long long)((js->helperCalls - jitHelper) / framesSincePerf) : 0ull);
                if (js)
                {
                    u64 ins = sys->cpu.instructions - insnsPrev, idle = js->idleSkipped - jitIdle;
                    static u64 linksPrev, unlinksPrev, staleLast;
                    H64_INFO("[jit] per frame: %llu instructions executed, %llu skipped idle, %llu blocks run, %llu interpreter steps, "
                             "%llu links made, %llu TLB checks failed, %llu remapped blocks unlinked",
                             (unsigned long long)((ins - idle) / framesSincePerf), (unsigned long long)(idle / framesSincePerf),
                             (unsigned long long)((js->blocksRun - jitRun) / framesSincePerf),
                             (unsigned long long)((js->interpSteps - jitSteps) / framesSincePerf),
                             (unsigned long long)((js->linksMade - linksPrev) / framesSincePerf),
                             (unsigned long long)((js->tlbStale - staleLast) / framesSincePerf),
                             (unsigned long long)((js->tlbUnlinks - unlinksPrev) / framesSincePerf));
                    staleLast = js->tlbStale;
                    linksPrev = js->linksMade;
                    unlinksPrev = js->tlbUnlinks;
                    jitBlocks = js->blocksCompiled; jitInval = js->invalidations; jitFlush = js->flushes; jitHelper = js->helperCalls;
                    jitRun = js->blocksRun; jitIdle = js->idleSkipped; jitSteps = js->interpSteps;
                }
                insnsPrev = sys->cpu.instructions;
                if (xs.bigW)
                    H64_INFO("[xtex] largest decode %ux%u (%u of 64K+ texels) fmt %u size %u stride %u mask %u/%u flags %X from a %s",
                             xs.bigW, xs.bigH, xs.bigCount, xs.bigFmt, xs.bigSize, xs.bigStride, xs.bigMaskS, xs.bigMaskT, xs.bigFlags,
                             xs.bigRect ? "rectangle" : "triangle");
                PcSamplerReport();
                H64_INFO("[gprof] ms/frame on the graphics worker: snapshots %.1f (%u KB a frame), vertices %.1f (%u a frame; lighting %.1f, %u lights a frame), TMEM loads %u (%u KB, %.1f ms), palettes %u (%.1f ms)",
                         pr[H64_PROF_GFX_SNAPSHOT] * k, (u32)(sys->prof[H64_PROF_SNAP_BYTES] / framesSincePerf / 1024),
                         pr[H64_PROF_GFX_VTX] * k, (u32)(sys->prof[H64_PROF_GFX_NVTX] / framesSincePerf),
                         pr[H64_PROF_GFX_LIGHT] * k, (u32)(sys->prof[H64_PROF_GFX_NLIGHT] / framesSincePerf),
                         (u32)(sys->prof[H64_PROF_TMEM_LOADS] / framesSincePerf), (u32)(sys->prof[H64_PROF_TMEM_WORDS] * 8 / framesSincePerf / 1024),
                         pr[H64_PROF_TMEM_TIME] * k, (u32)(sys->prof[H64_PROF_TMEM_NTLUT] / framesSincePerf), pr[H64_PROF_TMEM_TLUT] * k);
                H64_INFO("[cprof] ms/frame inside cpu: generated code %.1f (helper %.1f), scheduler events %.1f, TLB changes %.1f; waiting for the graphics worker %.1f (task end %.1f, RDP via DPC %.1f, task start %.1f)%s",
                         pr[H64_PROF_BLOCKS] * k, pr[H64_PROF_HELPER] * k, pr[H64_PROF_EVENTS] * k, pr[H64_PROF_TLB] * k, pr[H64_PROF_ASYNC_WAIT] * k,
                         pr[H64_PROF_WAIT_TASK] * k, pr[H64_PROF_WAIT_DPC] * k, pr[H64_PROF_WAIT_START] * k,
                         s_gfxQ.thread ? " (graphics and audio HLE run on workers, beside cpu)" : "");
                H64_INFO("[xprof] ms/frame: rdp commands %.1f (state %.1f, textures %.1f: keys %.1f, decode %.1f [create %.1f lock %.1f decode %.1f], %u texels/frame, %u cache resets, arena %u textures %u chunk reuses, %u rectangles from GPU frames) draw calls %.1f, copy-backs %.1f (GPU wait %.1f), TMEM loads %.1f, %u resolves (%u copy backs)",
                         xs.tRdp * k, xs.tState * k, xs.tTexture * k, xs.tHash * k, xs.tDecode * k, xs.tCreate * k, xs.tLock * k,
                         xs.tFill * k, xs.texelsDecoded / framesSincePerf, xs.retires, xs.arenaTextures, xs.arenaEvictions, xs.fbTexRects, xs.tDraw * k, xs.tCopyBack * k, xs.tCopyBackWait * k, xs.tLoads * k, xs.loadResolves, xs.resolveCopyBacks);
            }
            memset(sys->prof, 0, sizeof(sys->prof));
            profRun = profPresent = profWait = 0;
            perfStart = perfNow;   // the next period starts now, the time spent logging included
            s_lastPerfBlock = QpcMs() - perfNow;
            framesSincePerf = 0;
            instrAtPerf = sys->cpu.instructions;
        }
    }
    WorkersStop(sys);
    SavesTick(sys, presented, 1);
    LogFlush();
    sys->renderer = NULL;
    h64_xenos_free(renderer);
    xb_audio_shutdown();
    s_samplePc = NULL;
    Sleep(5);
    FreeSystem(sys);
    H64_INFO("[main] game ended: %s", result == RG_BROWSER ? "ROM list" : "dashboard");
    LogFlush();
    if (s_crashed) MessageScreen(dev, "The game crashed", s_crashText, D3DCOLOR_XRGB(240, 60, 60));
    return result;
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

    InitializeCriticalSection(&s_logLock);
    InitializeCriticalSection(&s_logFileLock);
    s_log = fopen("game:\\harissa64v2.log", "w");
    if (!s_log)
    {
        s_log = fopen("cache:\\harissa64v2.log", "w");
        s_logDrive = "cache";
    }
    h64_log_set_sink(file_sink);
    s_logThread = CreateThread(NULL, 64 * 1024, LogThread, NULL, CREATE_SUSPENDED, NULL);
    if (s_logThread)
    {
        XSetThreadProcessor(s_logThread, 5);
        SetThreadPriority(s_logThread, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(s_logThread);
    }
    H64_INFO("[main] Harissa64 V2 %s (Xbox 360), %s-endian, %d-bit pointers", H64_VERSION_STRING,
             H64_HOST_BIG_ENDIAN ? "big" : "little", (int)(sizeof(void *) * 8));
    LoadConfig(&cfg);
    if (cfg.xenia || !cfg.fpuFlags) h64_fenv_disable_host_flags();
    s_jitNoFpu = !cfg.jitFpu;
    s_noRegCache = !cfg.regCache;
    s_noFpCache = !cfg.fpCache;
    s_fastFpu = cfg.fastFpu;
    s_fullExits = cfg.fullExits;
    s_noSuper = !cfg.superblocks;
    s_cpi = cfg.cpi >= 1 && cfg.cpi <= 8 ? (u32)cfg.cpi : 2;
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
            dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
            UiBegin(dev);
            UiBackdrop();
            UiBrand(UI_SAFE_X, 72, 34);
            UiText(UI_REGULAR, 26, UI_WIDTH * 0.5f, UI_HEIGHT * 0.5f, UI_COL_TEXT, "Recompiler test running" UI_CH_ELLIPSIS, UI_CENTER);
            UiEnd();
            dev->Present(NULL, NULL, NULL, NULL);
        }
        RunDynarecTest(&cfg, jitReport, sizeof(jitReport));
        MessageScreen(dev, status, jitReport,
                      strstr(jitReport, "DIVERGED") ? D3DCOLOR_XRGB(240, 60, 60) : D3DCOLOR_XRGB(200, 200, 120));
    }
    else if (dev)
    {
        // Scripted runs and rom= (or game:\test.z64) start one game directly
        // with harissa64v2.ini alone; otherwise the ROM browser, with the
        // menu's settings and the game's profile over harissa64v2.ini.
        int scripted = cfg.input.count || cfg.exitAfter || cfg.trace || cfg.shotCount || cfg.loadState || cfg.saveStateAt;
        if (scripted || cfg.rom[0] || GetFileAttributesA("game:\\test.z64") != 0xFFFFFFFF)
        {
            s_settingsPath[0] = s_profilePath[0] = 0;
            RunGame(dev, &cfg, NULL);
        }
        else
        {
            static Config base, game;
            char dir[64], path[256], folder[64];
            _snprintf(dir, sizeof(dir), "%s:\\config", s_logDrive);
            dir[sizeof(dir) - 1] = 0;
            CreateDirectoryA(dir, NULL);
            _snprintf(s_settingsPath, sizeof(s_settingsPath), "%s\\settings.ini", dir);
            s_settingsPath[sizeof(s_settingsPath) - 1] = 0;
            for (;;)
            {
                base = cfg;
                ConfigParseFile(&base, s_settingsPath);
                if (!RomBrowser(dev, &base, s_settingsPath, path, sizeof(path))) break;
                if (cfg.autoStart > 0) { cfg.autoStart--; cfg.autoIndex++; }
                game = base;
                s_profilePath[0] = 0;
                if (RomFolderName(path, folder, sizeof(folder)))
                {
                    _snprintf(s_profilePath, sizeof(s_profilePath), "%s\\%s.ini", dir, folder);
                    s_profilePath[sizeof(s_profilePath) - 1] = 0;
                    if (ConfigParseFile(&game, s_profilePath)) H64_INFO("[main] game profile %s", s_profilePath);
                }
                if (RunGame(dev, &game, path) == RG_DASHBOARD) break;
            }
        }
    }

    if (s_logThread)
    {
        s_logQuit = 1;
        WaitForSingleObject(s_logThread, INFINITE);
        CloseHandle(s_logThread);
        s_logThread = NULL;
    }
    LogFlush();
    if (s_log)
        fclose(s_log);
    // Back to the dashboard (Aurora on the console). Xenia has none: it looks
    // for game:\default.xex and shows "Title Launch Failed", so with xenia=1
    // the title just ends.
    if (!cfg.xenia)
        XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    return 0;
}
