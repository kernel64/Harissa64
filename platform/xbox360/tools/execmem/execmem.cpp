// Harissa64 V2 - M0.3: executable memory proof of concept.
//
// Tries every plausible way of getting memory the title can write machine
// code into and then execute. For each method: allocate, write
// "li r3,42; blr", flush the data cache and invalidate the instruction cache,
// call it, rewrite it as "li r3,43; blr", flush again, call it again.
//
// A method can kill the title outright (the first console run died inside
// method 1 without the __except handler firing). So the program survives
// crashes across launches:
//  - execmem_state.txt holds the index of the next method to try; it is
//    advanced *before* a method runs, so a relaunch skips the one that
//    crashed;
//  - execmem_results.txt gets one line per method that finished;
//  - execmem.log gets a line before every step (alloc, write, flush, call),
//    written through to the disk.
// Relaunch after a crash until every method has run. The screen shows one bar
// per method from all launches: green = 42 then 43, red = allocation failed,
// magenta = write faulted, orange = call faulted, yellow = wrong value, dark
// red = crashed the title, grey = not run yet. Y resets everything, Back
// returns to the dashboard. Files go to game:\ (cache:\ when read-only).

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ppcintrinsics.h>

// Kernel exports from xboxkrnl.lib (not declared in the public headers).
extern "C"
{
    VOID NTAPI KeSweepIcacheRange(PVOID Address, SIZE_T Size);
    VOID NTAPI MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect);
    PVOID NTAPI MmAllocatePhysicalMemoryEx(ULONG Flags, SIZE_T NumberOfBytes, ULONG Protect,
                                           ULONG_PTR LowestAcceptableAddress, ULONG_PTR HighestAcceptableAddress,
                                           ULONG_PTR Alignment);
    LONG NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, SIZE_T *RegionSize, ULONG AllocationType,
                                       ULONG Protect, ULONG Unknown);
}

#define CODE_BYTES 4096
#define NUM_METHODS 10

static char g_dir[16] = "game:\\";

// ---- Files written through to the disk (they must survive a crash) ----
static void AppendFile(const char *name, const char *text)
{
    char path[64];
    HANDLE h;
    DWORD written;
    _snprintf(path, sizeof(path), "%s%s", g_dir, name);
    path[sizeof(path) - 1] = 0;
    h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static void WriteWholeFile(const char *name, const char *text)
{
    char path[64];
    HANDLE h;
    DWORD written;
    _snprintf(path, sizeof(path), "%s%s", g_dir, name);
    path[sizeof(path) - 1] = 0;
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static int ReadWholeFile(const char *name, char *buf, int size)
{
    char path[64];
    HANDLE h;
    DWORD got = 0;
    _snprintf(path, sizeof(path), "%s%s", g_dir, name);
    path[sizeof(path) - 1] = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    buf[0] = 0;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ReadFile(h, buf, (DWORD)(size - 1), &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    return (int)got;
}

static void DeleteOne(const char *name)
{
    char path[64];
    _snprintf(path, sizeof(path), "%s%s", g_dir, name);
    path[sizeof(path) - 1] = 0;
    DeleteFileA(path);
}

static void Log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 3] = 0;
    strcat(buf, "\r\n");
    OutputDebugStringA(buf);
    AppendFile("execmem.log", buf);
}

// ---- Results ----
enum Status { ST_NOT_RUN, ST_ALLOC_FAIL, ST_WRITE_FAULT, ST_CALL_FAULT, ST_WRONG, ST_OK, ST_CRASHED };
static const char *s_statusName[] = { "not-run", "alloc-failed", "write-fault", "call-fault", "wrong-value", "OK",
                                      "CRASHED" };
static const D3DCOLOR s_statusColor[] = {
    D3DCOLOR_XRGB(90, 90, 90), D3DCOLOR_XRGB(200, 30, 30), D3DCOLOR_XRGB(200, 40, 200),
    D3DCOLOR_XRGB(240, 130, 20), D3DCOLOR_XRGB(230, 220, 30), D3DCOLOR_XRGB(30, 200, 60),
    D3DCOLOR_XRGB(110, 0, 0) };

// ---- Cache flush ----
// One cache line: "dcbst 0,r3; sync; icbi 0,r3". The XDK has no icbi
// intrinsic and no inline assembly, so the words are emitted raw; this works
// because the argument arrives in r3 and nothing runs before them (checked
// with dumpbin /disasm).
static __declspec(noinline) void FlushLine(void *p)
{
    __emit(0x7C00186C);   // dcbst 0,r3
    __emit(0x7C0004AC);   // sync
    __emit(0x7C001FAC);   // icbi 0,r3
}

static int g_useKernelSweep = 0;

static void FlushCode(void *p, SIZE_T size)
{
    SIZE_T i;
    if (g_useKernelSweep)
    {
        for (i = 0; i < size; i += 128)
            __dcbst((int)i, p);
        __sync();
        KeSweepIcacheRange(p, size);
    }
    else
    {
        for (i = 0; i < size; i += 128)
            FlushLine((char *)p + i);
        __sync();
    }
    __emit(0x4C00012C);   // isync
}

typedef int (*CodeFn)(void);

static int WriteCode(void *p, int value, DWORD *exc)
{
    __try
    {
        unsigned int *w = (unsigned int *)p;
        w[0] = 0x38600000u | (unsigned int)(value & 0xFFFF);   // li r3, value
        w[1] = 0x4E800020u;                                     // blr
        return 1;
    }
    __except (*exc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int CallCode(void *p, int *result, DWORD *exc)
{
    __try
    {
        *result = ((CodeFn)p)();
        return 1;
    }
    __except (*exc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// ---- Methods ----
static void ProtRW_VP(void *p)  { DWORD old; VirtualProtect(p, CODE_BYTES, PAGE_READWRITE, &old); }
static void ProtRX_VP(void *p)  { DWORD old; VirtualProtect(p, CODE_BYTES, PAGE_EXECUTE_READ, &old); }

// Method 9: a section of the image that is writable and executable
// (linked with /SECTION:.jitc,ERW).
#pragma section(".jitc", read, write, execute)
__declspec(allocate(".jitc")) static unsigned int s_sectionCode[CODE_BYTES / 4] = { 1 };

static void *Alloc(int m)
{
    void *p;
    switch (m)
    {
    case 0: return VirtualAlloc(NULL, CODE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    case 1: return VirtualAlloc(NULL, 64 * 1024, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_EXECUTE_READWRITE);
    case 2: return VirtualAlloc(NULL, CODE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    case 3:
        p = VirtualAlloc(NULL, CODE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p) { Log("  MmSetAddressProtect(RWX)..."); MmSetAddressProtect(p, CODE_BYTES, PAGE_EXECUTE_READWRITE); }
        return p;
    case 4: return XPhysicalAlloc(CODE_BYTES, MAXULONG_PTR, 0, PAGE_EXECUTE_READWRITE);
    case 5:
        p = XPhysicalAlloc(CODE_BYTES, MAXULONG_PTR, 0, PAGE_READWRITE);
        if (p) { Log("  MmSetAddressProtect(RWX)..."); MmSetAddressProtect(p, CODE_BYTES, PAGE_EXECUTE_READWRITE); }
        return p;
    case 6: return MmAllocatePhysicalMemoryEx(0, CODE_BYTES, PAGE_EXECUTE_READWRITE, 0, 0xFFFFFFFF, 4096);
    case 7:
    {
        PVOID base = NULL;
        SIZE_T size = CODE_BYTES;
        LONG st = NtAllocateVirtualMemory(&base, &size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE, 0);
        Log("  NtAllocateVirtualMemory status %08lX", st);
        return st >= 0 ? base : NULL;
    }
    case 8: return s_sectionCode;
    case 9: return VirtualAlloc(NULL, CODE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    return NULL;
}

static const char *s_methodName[NUM_METHODS] = {
    "1 VirtualAlloc RWX",
    "2 VirtualAlloc RWX large pages",
    "3 VirtualAlloc RW + VirtualProtect RX",
    "4 VirtualAlloc RW + MmSetAddressProtect",
    "5 XPhysicalAlloc RWX",
    "6 XPhysicalAlloc RW + MmSetAddressProtect",
    "7 MmAllocatePhysicalMemoryEx RWX",
    "8 NtAllocateVirtualMemory RWX",
    "9 image section .jitc (RWX)",
    "10 VirtualAlloc RWX + KeSweepIcacheRange",
};

static Status RunMethod(int m)
{
    void *p;
    int r1 = 0, r2 = 0, wx = (m == 2);
    DWORD exc = 0;

    Log("%s: alloc...", s_methodName[m]);
    p = Alloc(m);
    if (!p) { Log("  alloc failed (GetLastError=%lu)", GetLastError()); return ST_ALLOC_FAIL; }
    Log("  alloc %p; write...", p);
    if (wx) ProtRW_VP(p);
    if (!WriteCode(p, 42, &exc)) { Log("  write faulted, exception %08lX", exc); return ST_WRITE_FAULT; }
    if (wx) { Log("  VirtualProtect(RX)..."); ProtRX_VP(p); }
    Log("  flush...");
    FlushCode(p, CODE_BYTES);
    Log("  call...");
    if (!CallCode(p, &r1, &exc)) { Log("  call faulted, exception %08lX", exc); return ST_CALL_FAULT; }
    Log("  returned %d; rewrite...", r1);
    if (wx) ProtRW_VP(p);
    if (!WriteCode(p, 43, &exc)) { Log("  rewrite faulted, exception %08lX", exc); return ST_WRITE_FAULT; }
    if (wx) ProtRX_VP(p);
    Log("  flush...");
    FlushCode(p, CODE_BYTES);
    Log("  call...");
    if (!CallCode(p, &r2, &exc)) { Log("  second call faulted, exception %08lX", exc); return ST_CALL_FAULT; }
    Log("  returned %d", r2);
    return (r1 == 42 && r2 == 43) ? ST_OK : ST_WRONG;
}

// ---- Screen text through Clear rectangles (no shaders, no font files) ----
#include "../../font8x8_basic.h"
#define MAX_RECTS 6000
static D3DRECT s_rects[MAX_RECTS];
static int s_rectCount;

static void AddText(int x, int y, int scale, const char *text)
{
    for (; *text; text++, x += 8 * scale)
    {
        unsigned char c = (unsigned char)*text;
        int row, col;
        if (c >= 128) c = '?';
        for (row = 0; row < 8; row++)
            for (col = 0; col < 8; col++)
                if ((font8x8_basic[c][row] & (1 << col)) && s_rectCount < MAX_RECTS)
                {
                    D3DRECT *r = &s_rects[s_rectCount++];
                    r->x1 = x + col * scale; r->y1 = y + row * scale;
                    r->x2 = r->x1 + scale;   r->y2 = r->y1 + scale;
                }
    }
}

static void FlushText(IDirect3DDevice9 *dev, D3DCOLOR color)
{
    if (s_rectCount)
        dev->Clear(s_rectCount, s_rects, D3DCLEAR_TARGET, color, 1.0f, 0);
    s_rectCount = 0;
}

int __cdecl main()
{
    Status status[NUM_METHODS];
    char buf[2048], line[128];
    int next, i;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    WORD prevButtons = 0;

    // game:\ if writable, else cache:\ (Xenia).
    {
        HANDLE h = CreateFileA("game:\\execmem_probe.tmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) strcpy(g_dir, "cache:\\");
        else { CloseHandle(h); DeleteFileA("game:\\execmem_probe.tmp"); }
    }

restart:
    for (i = 0; i < NUM_METHODS; i++)
        status[i] = ST_NOT_RUN;
    ReadWholeFile("execmem_state.txt", buf, sizeof(buf));
    next = atoi(buf);
    if (next < 0 || next > NUM_METHODS) next = 0;
    // Methods before `next` without a result line crashed the title.
    for (i = 0; i < next; i++)
        status[i] = ST_CRASHED;
    ReadWholeFile("execmem_results.txt", buf, sizeof(buf));
    {
        char *s = buf;
        while (*s)
        {
            int m = -1, st = -1;
            if (sscanf(s, "%d %d", &m, &st) == 2 && m >= 0 && m < NUM_METHODS && st >= 0 && st <= ST_CRASHED)
                status[m] = (Status)st;
            while (*s && *s != '\n') s++;
            if (*s) s++;
        }
    }
    if (next == 0)
        Log("==== Harissa64 V2 execmem POC, fresh start ====");
    else if (next < NUM_METHODS)
        Log("==== relaunched: resuming at method %d (method %d did not finish) ====", next + 1, next);

    for (; next < NUM_METHODS; next++)
    {
        Status st;
        _snprintf(line, sizeof(line), "%d", next + 1);
        WriteWholeFile("execmem_state.txt", line);   // a crash in this method moves on to the next
        g_useKernelSweep = (next == 9);
        st = RunMethod(next);
        g_useKernelSweep = 0;
        status[next] = st;
        _snprintf(line, sizeof(line), "%d %d %s\r\n", next, (int)st, s_statusName[st]);
        AppendFile("execmem_results.txt", line);
        Log("%s -> %s", s_methodName[next], s_statusName[st]);
    }
    {
        int ok = 0;
        buf[0] = 0;
        for (i = 0; i < NUM_METHODS; i++)
        {
            if (status[i] == ST_OK) ok++;
            _snprintf(line, sizeof(line), "%d:%s ", i + 1, s_statusName[status[i]]);
            strcat(buf, line);
        }
        Log("SUMMARY %d/%d OK: %s", ok, NUM_METHODS, buf);
    }

    if (!dev)
    {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        ZeroMemory(&pp, sizeof(pp));
        pp.BackBufferWidth = 1280;
        pp.BackBufferHeight = 720;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        if (d3d) d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    }

    for (;;)
    {
        XINPUT_STATE in;
        WORD buttons = 0, pressed;
        if (dev)
        {
            dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(10, 12, 20), 1.0f, 0);
            AddText(80, 30, 3, "HARISSA64 V2 - EXECUTABLE MEMORY TEST");
            FlushText(dev, D3DCOLOR_XRGB(220, 220, 220));
            for (i = 0; i < NUM_METHODS; i++)
            {
                D3DRECT r;
                r.x1 = 80; r.x2 = 120; r.y1 = 90 + i * 52; r.y2 = r.y1 + 40;
                dev->Clear(1, &r, D3DCLEAR_TARGET, s_statusColor[status[i]], 1.0f, 0);
                _snprintf(line, sizeof(line), "%-42s %s", s_methodName[i], s_statusName[status[i]]);
                line[sizeof(line) - 1] = 0;
                AddText(140, 102 + i * 52, 2, line);
            }
            FlushText(dev, D3DCOLOR_XRGB(230, 230, 230));
            AddText(80, 640, 2, "BACK: dashboard    Y: reset and run all methods again");
            FlushText(dev, D3DCOLOR_XRGB(140, 140, 160));
            dev->Present(NULL, NULL, NULL, NULL);
        }
        if (XInputGetState(0, &in) == ERROR_SUCCESS)
            buttons = in.Gamepad.wButtons;
        pressed = (WORD)(buttons & ~prevButtons);
        prevButtons = buttons;
        if (pressed & XINPUT_GAMEPAD_BACK)
            break;
        if (pressed & XINPUT_GAMEPAD_Y)
        {
            DeleteOne("execmem_state.txt");
            DeleteOne("execmem_results.txt");
            Log("==== reset by the user ====");
            goto restart;
        }
        Sleep(16);
    }
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    return 0;
}
