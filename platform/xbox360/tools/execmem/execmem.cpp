// Harissa64 V2 - M0.3: executable memory proof of concept.
//
// Tries every plausible way of getting memory the title can write machine
// code into and then execute. For each method: allocate, write
// "li r3,42; blr", flush the data cache and invalidate the instruction cache,
// call it, rewrite it as "li r3,43; blr", flush again, call it again.
//
// Results: one bar per method on screen (grey = not run, red = allocation
// failed, magenta = write faulted, orange = call faulted, yellow = wrong
// value, green = 42 then 43), a summary in a system message box, and the
// details in game:\execmem.log (falls back to cache:\execmem.log).

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <ppcintrinsics.h>

// Kernel exports from xboxkrnl.lib (not declared in the public headers).
extern "C"
{
    VOID NTAPI KeSweepIcacheRange(PVOID Address, SIZE_T Size);
    VOID NTAPI KeSweepDcacheRange(PVOID Address, SIZE_T Size);
    VOID NTAPI MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect);
    ULONG NTAPI MmQueryAddressProtect(PVOID VirtualAddress);
    PVOID NTAPI MmAllocatePhysicalMemoryEx(ULONG Flags, SIZE_T NumberOfBytes, ULONG Protect,
                                           ULONG_PTR LowestAcceptableAddress, ULONG_PTR HighestAcceptableAddress,
                                           ULONG_PTR Alignment);
    LONG NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, SIZE_T *RegionSize, ULONG AllocationType,
                                       ULONG Protect, ULONG Unknown);
}

#define CODE_BYTES 4096

static FILE *g_log = NULL;
static void Log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

enum Status { ST_NOT_RUN, ST_ALLOC_FAIL, ST_WRITE_FAULT, ST_CALL_FAULT, ST_WRONG, ST_OK };
static const char *s_statusName[] = { "not run", "ALLOC FAILED", "WRITE FAULT", "CALL FAULT", "WRONG VALUE", "OK" };
static const D3DCOLOR s_statusColor[] = {
    D3DCOLOR_XRGB(90, 90, 90), D3DCOLOR_XRGB(200, 30, 30), D3DCOLOR_XRGB(200, 40, 200),
    D3DCOLOR_XRGB(240, 130, 20), D3DCOLOR_XRGB(230, 220, 30), D3DCOLOR_XRGB(30, 200, 60) };

struct Method
{
    const char *name;
    Status status;
    int first, second;
    void *addr;
    DWORD exc;
};

typedef int (*CodeFn)(void);

// One cache line: "dcbst 0,r3; sync; icbi 0,r3". The XDK has no icbi
// intrinsic and no inline assembly, so the instructions are emitted raw; the
// function only works because its argument arrives in r3 and nothing else
// runs before the emitted words (checked in the disassembly).
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

// beforeWrite/afterWrite let a method switch protections around writes (W^X).
typedef void (*ProtectFn)(void *p);

static void RunMethod(Method *m, void *p, ProtectFn beforeWrite, ProtectFn afterWrite)
{
    int r = 0;
    m->addr = p;
    if (!p) { m->status = ST_ALLOC_FAIL; Log("%-34s alloc failed (GetLastError=%lu)", m->name, GetLastError()); return; }
    Log("%-34s alloc %p", m->name, p);

    if (beforeWrite) beforeWrite(p);
    if (!WriteCode(p, 42, &m->exc)) { m->status = ST_WRITE_FAULT; Log("  write faulted, exception %08lX", m->exc); return; }
    if (afterWrite) afterWrite(p);
    FlushCode(p, CODE_BYTES);
    if (!CallCode(p, &r, &m->exc)) { m->status = ST_CALL_FAULT; Log("  call faulted, exception %08lX", m->exc); return; }
    m->first = r;

    if (beforeWrite) beforeWrite(p);
    if (!WriteCode(p, 43, &m->exc)) { m->status = ST_WRITE_FAULT; Log("  rewrite faulted, exception %08lX", m->exc); return; }
    if (afterWrite) afterWrite(p);
    FlushCode(p, CODE_BYTES);
    if (!CallCode(p, &r, &m->exc)) { m->status = ST_CALL_FAULT; Log("  second call faulted, exception %08lX", m->exc); return; }
    m->second = r;

    m->status = (m->first == 42 && m->second == 43) ? ST_OK : ST_WRONG;
    Log("  results %d then %d -> %s", m->first, m->second, s_statusName[m->status]);
}

static void ProtRW_VP(void *p)  { DWORD old; VirtualProtect(p, CODE_BYTES, PAGE_READWRITE, &old); }
static void ProtRX_VP(void *p)  { DWORD old; VirtualProtect(p, CODE_BYTES, PAGE_EXECUTE_READ, &old); }
static void ProtRWX_Mm(void *p) { MmSetAddressProtect(p, CODE_BYTES, PAGE_EXECUTE_READWRITE); }

// Method 9: a section of the image that is writable and executable.
#pragma section(".jitc", read, write, execute)
__declspec(allocate(".jitc")) static unsigned int s_sectionCode[CODE_BYTES / 4] = { 1 };

#define NUM_METHODS 10

int __cdecl main()
{
    Method m[NUM_METHODS];
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    char summary[600];
    WCHAR wsummary[600];
    int i, ok = 0;

    g_log = fopen("game:\\execmem.log", "w");
    if (!g_log) g_log = fopen("cache:\\execmem.log", "w");
    Log("Harissa64 V2 execmem POC");

    memset(m, 0, sizeof(m));
    m[0].name = "1 VirtualAlloc RWX";
    m[1].name = "2 VirtualAlloc RWX large pages";
    m[2].name = "3 VirtualAlloc RW + VirtualProtect RX";
    m[3].name = "4 VirtualAlloc RW + MmSetAddressProtect";
    m[4].name = "5 XPhysicalAlloc RWX";
    m[5].name = "6 XPhysicalAlloc RW + MmSetAddrProt";
    m[6].name = "7 MmAllocatePhysicalMemoryEx RWX";
    m[7].name = "8 NtAllocateVirtualMemory RWX";
    m[8].name = "9 image section .jitc (RWX)";
    m[9].name = "10 VirtualAlloc RWX + KeSweepIcacheRange";

    RunMethod(&m[0], VirtualAlloc(NULL, CODE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), NULL, NULL);
    RunMethod(&m[1], VirtualAlloc(NULL, 64 * 1024, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_EXECUTE_READWRITE), NULL, NULL);
    RunMethod(&m[2], VirtualAlloc(NULL, CODE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), ProtRW_VP, ProtRX_VP);
    {
        void *p = VirtualAlloc(NULL, CODE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p) ProtRWX_Mm(p);
        RunMethod(&m[3], p, NULL, NULL);
    }
    RunMethod(&m[4], XPhysicalAlloc(CODE_BYTES, MAXULONG_PTR, 0, PAGE_EXECUTE_READWRITE), NULL, NULL);
    {
        void *p = XPhysicalAlloc(CODE_BYTES, MAXULONG_PTR, 0, PAGE_READWRITE);
        if (p) ProtRWX_Mm(p);
        RunMethod(&m[5], p, NULL, NULL);
    }
    RunMethod(&m[6], MmAllocatePhysicalMemoryEx(0, CODE_BYTES, PAGE_EXECUTE_READWRITE, 0, 0xFFFFFFFF, 4096), NULL, NULL);
    {
        PVOID base = NULL;
        SIZE_T size = CODE_BYTES;
        LONG st = NtAllocateVirtualMemory(&base, &size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE, 0);
        Log("NtAllocateVirtualMemory status %08lX", st);
        RunMethod(&m[7], st >= 0 ? base : NULL, NULL, NULL);
    }
    RunMethod(&m[8], s_sectionCode, NULL, NULL);
    // Last: Xenia does not implement KeSweepIcacheRange (the call crashes there).
    g_useKernelSweep = 1;
    RunMethod(&m[9], VirtualAlloc(NULL, CODE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), NULL, NULL);
    g_useKernelSweep = 0;

    summary[0] = 0;
    for (i = 0; i < NUM_METHODS; i++)
    {
        char line[96];
        if (m[i].status == ST_OK) ok++;
        _snprintf(line, sizeof(line), "%d:%s ", i + 1, m[i].status == ST_OK ? "OK" :
                  m[i].status == ST_ALLOC_FAIL ? "alloc" : m[i].status == ST_WRITE_FAULT ? "wfault" :
                  m[i].status == ST_CALL_FAULT ? "xfault" : m[i].status == ST_WRONG ? "wrong" : "-");
        strcat(summary, line);
    }
    Log("SUMMARY %d/%d OK: %s", ok, NUM_METHODS, summary);
    if (g_log) fclose(g_log), g_log = NULL;

    // Screen: one bar per method.
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (d3d) d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);

    MultiByteToWideChar(CP_ACP, 0, summary, -1, wsummary, 600);
    {
        XOVERLAPPED ov;
        MESSAGEBOX_RESULT res;
        LPCWSTR buttons[1] = { L"OK" };
        WCHAR title[64];
        DWORD mb;
        int shown = 0;
        ZeroMemory(&ov, sizeof(ov));
        _snwprintf(title, 64, L"Harissa64 V2 execmem: %d/%d OK", ok, NUM_METHODS);
        for (;;)
        {
            if (dev)
            {
                D3DRECT r;
                dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(10, 12, 20), 1.0f, 0);
                for (i = 0; i < NUM_METHODS; i++)
                {
                    r.x1 = 160; r.x2 = 1120;
                    r.y1 = 80 + i * 64; r.y2 = r.y1 + 44;
                    dev->Clear(1, &r, D3DCLEAR_TARGET, s_statusColor[m[i].status], 1.0f, 0);
                    // a white tick per method number, left of the bar
                    r.x1 = 100; r.x2 = 100 + 4 * (i + 1);
                    dev->Clear(1, &r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255), 1.0f, 0);
                }
                dev->Present(NULL, NULL, NULL, NULL);
            }
            if (!shown)
            {
                mb = XShowMessageBoxUI(0, title, wsummary, 1, buttons, 0, XMB_ALERTICON, &res, &ov);
                shown = (mb == ERROR_IO_PENDING || mb == ERROR_SUCCESS) ? 1 : -1;
            }
            else if (shown == 1 && XHasOverlappedIoCompleted(&ov))
                shown = 2;
            {
                XINPUT_STATE in;
                if (shown != 1 && XInputGetState(0, &in) == ERROR_SUCCESS &&
                    (in.Gamepad.wButtons & XINPUT_GAMEPAD_BACK))
                    break;
            }
            Sleep(16);
        }
    }
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    return 0;
}
