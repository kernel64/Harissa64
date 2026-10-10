// h64test - Harissa64 V2 headless runner.
//
//   h64test --unit                       run the unit tests
//   h64test --rom FILE [options]         run a ROM
//     --frames N          stop after N VI frames (default 600)
//     --seconds S         stop after S emulated seconds (overrides --frames)
//     --dillon            stop when r30 becomes non-zero (Dillonb/n64-tests: -1 = pass)
//     --until TEXT        stop when an ISViewer line contains TEXT
//     --info              print the ROM header and exit
//     --state             print the CPU state at the end
//     --trace-exc N       print the first N exceptions (not interrupts)
//     --fb-png FILE       write the framebuffer the VI shows at the end (raw, no VI filtering)
//   --verbose             debug log level
// Exit code: 0 normally, 1 for a failed unit test or Dillonb test, 2 for bad usage.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_types.h"
#include "../../core/common/h64_endian.h"
#include "../../core/cart/h64_zip.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../core/system/h64_system.h"
#include "../../core/dynarec/h64_lockstep.h"
#include "../../core/vi/h64_vi.h"
#include "../../core/rdp/h64_rdp.h"
#include "../../core/rdp/h64_rdp_state.h"
#include "../../core/pif/h64_input_script.h"
#include "../../core/savestate/h64_state.h"
#include "../../core/common/h64_fenv.h"
#include "../../render/api.h"
#include "png_write.h"
#include "../../tests/unit/unit_tests.h"

static void stdout_sink(int level, const char *line)
{
    FILE *f = level <= H64_LOG_WARN ? stderr : stdout;
    fprintf(f, "%s\n", line);
    fflush(f);
}

// ---- Executable memory for the recompiler (ppc64 Linux, used under QEMU) ----
#if H64_JIT_CAN_RUN && defined(__linux__)
#include <sys/mman.h>
static void *exec_alloc(u32 size)
{
    void *p = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
static void flush_icache(void *addr, u32 len) { __builtin___clear_cache((char *)addr, (char *)addr + len); }
#else
static void *exec_alloc(u32 size) { (void)size; return 0; }
static void flush_icache(void *addr, u32 len) { (void)addr; (void)len; }
#endif

#if defined(__linux__) && defined(__powerpc64__)
// Static ppc64 build: report where a stack-protector check failed (resolve
// the addresses with powerpc64-linux-gnu-addr2line -f -e build-ppc64/h64test).
extern "C" void __stack_chk_fail(void)
{
    fprintf(stderr, "STACK SMASH in the function returning to %p\n", __builtin_return_address(0));
    abort();
}
#endif

static int enable_jit(H64System *sys)
{
    const u32 size = 16u << 20;   // as on the Xbox
    void *mem = exec_alloc(size);
    if (!mem || h64_jit_init(sys, mem, size, flush_icache))
    {
        fprintf(stderr, "the recompiler needs a PowerPC host (ppc64 big-endian Linux or the Xbox 360)\n");
        return -1;
    }
    return 0;
}

static const char *s_untilText = 0;
static int s_untilHit = 0;

static void isv_sink(void *user, const char *line)
{
    H64System *sys = (H64System *)user;
    printf("[isv] %s\n", line);
    fflush(stdout);
    if (s_untilText && strstr(line, s_untilText))
    {
        s_untilHit = 1;
        sys->stop = 1;
    }
}

static int s_traceExc = 0;

// --trace-exc: prints the first N non-interrupt exceptions.
static unsigned long long s_excCount[32];   // every exception, by code (printed at the end)
static void exc_hook(void *user, int code)
{
    H64System *sys = (H64System *)user;
    H64Cpu *c = &sys->cpu;
    s_excCount[code & 31]++;
    if (code == 15 && s_excCount[15] <= 4)   // floating point: what trapped
        printf("[exc] FPE at pc %08X (delay slot %d), fcr31 %08X, last op %08X, instr #%llu\n", (u32)c->curPc, c->curInDelaySlot,
               (u32)c->fcr31, c->lastOp, (unsigned long long)c->instructions);
    if (s_traceExc <= 0) return;
    // Interrupts are only reported when EPC points outside RDRAM (a corrupted PC).
    if (code == EXC_INT && ((u32)c->cop0[CP0_EPC] & 0xFF800000u) == 0x80000000u) return;
    {
        u32 p, op = 0;
        if (h64_cpu_translate_debug(c, c->curPc, &p)) h64_bus_read32(sys, p, &op);
        printf("[exc] opcode %08X, last PCs:", op);
        {
            int k;
            for (k = 0; k < 32; k++)
                printf("%s%08X", (k % 8) ? " " : "\n[exc]   ", c->pcHistory[(c->pcHistoryPos + k) & 31]);
            printf("\n[exc]   last jumps:");
            for (k = 0; k < 16; k++)
                printf("%s%08X->%08X", (k % 4) ? " " : "\n[exc]   ", c->jumpFrom[(c->jumpPos + k) & 15], c->jumpTo[(c->jumpPos + k) & 15]);
            printf("\n");
        }
    }
    s_traceExc--;
    printf("[exc] code %d at pc %08X%s, badvaddr %08X%08X, ra %08X, sr %08X, instr #%llu\n", code, (u32)c->curPc,
           c->curInDelaySlot ? " (delay slot)" : "", (u32)(c->cop0[CP0_BADVADDR] >> 32), (u32)c->cop0[CP0_BADVADDR],
           (u32)c->gpr[31], (u32)c->cop0[CP0_STATUS], (unsigned long long)c->instructions);
}

static int s_watchCount = 0;
static H64System *s_sysForStop = 0;
static int s_jumpStop = 0;

// --watch-pc: prints GPRs and 0x40 bytes at k0+0x100 the first 6 times PC is reached.
// --log-pc: one line per execution of that pc (ra, a0..a3), up to 400.
static int s_logPcCount;
static u32 s_logPcA0;   // --log-pc PC:A0: only when a0 matches
static u32 s_logWord;   // --log-word ADDR: that RDRAM word on each --log-pc line
static u32 s_logFrom;   // --log-from FRAME: --log-pc lines from that VI frame
static void log_pc_hook(void *user)
{
    H64System *sys = (H64System *)user;
    H64Cpu *c = &sys->cpu;
    if (sys->vi.frames < s_logFrom) return;
    if (s_logPcA0 && (u32)c->gpr[4] != s_logPcA0) return;
    if (s_logPcCount++ >= 400) return;
    printf("[logpc] %08X #%llu frame %u ra %08X a0 %08X a1 %08X a2 %08X a3 %08X sp %08X [%08X]\n", (u32)c->curPc, (unsigned long long)c->instructions,
           sys->vi.frames, (u32)c->gpr[31], (u32)c->gpr[4], (u32)c->gpr[5], (u32)c->gpr[6], (u32)c->gpr[7], (u32)c->gpr[29],
           s_logWord ? h64_load_be32(sys->rdram + (s_logWord & 0x7FFFFC)) : 0);
}

static void watch_hook(void *user)
{
    H64System *sys = (H64System *)user;
    H64Cpu *c = &sys->cpu;
    int i;
    {
        // Only report dispatches whose saved PC (k0+0x11C) is outside RDRAM.
        u32 p, epc = 0;
        if (h64_cpu_translate_debug(c, c->gpr[26] + 0x11C, &p)) h64_bus_read32(sys, p, &epc);
        if ((epc & 0xFF800000u) == 0x80000000u) return;
    }
    if (s_watchCount++ >= 3) return;
    printf("[watch] pc %08X #%llu:", (u32)c->curPc, (unsigned long long)c->instructions);
    for (i = 1; i < 32; i++)
        printf("%s r%d=%08X%08X", (i % 4) == 1 ? "\n[watch]  " : "", i, (u32)(c->gpr[i] >> 32), (u32)c->gpr[i]);
    printf("\n[watch]   mem[k0..k0+0x130]:");
    for (i = 0; i < 76; i++)
    {
        u32 p, v = 0;
        if (i % 8 == 0) printf("\n[watch]   +%03X:", 4 * i);
        if (h64_cpu_translate_debug(c, c->gpr[26] + 4 * i, &p)) h64_bus_read32(sys, p, &v);
        printf(" %08X", v);
    }
    printf("\n");
}

// --jump-limit: stops at the first jump/ERET to KSEG0 at or above the limit.
static void jump_hook(void *user, u32 from, u32 to)
{
    H64System *sys = (H64System *)user;
    H64Cpu *c = &sys->cpu;
    int k;
    printf("[jump] %08X -> %08X at instr #%llu, epc %08X, sr %08X, ra %08X, k0 %08X\n[jump] last PCs:", from, to,
           (unsigned long long)c->instructions, (u32)c->cop0[CP0_EPC], (u32)c->cop0[CP0_STATUS], (u32)c->gpr[31], (u32)c->gpr[26]);
    for (k = 0; k < 32; k++)
        printf("%s%08X", (k % 8) ? " " : "\n[jump]   ", c->pcHistory[(c->pcHistoryPos + k) & 31]);
    printf("\n");
    sys->stop = 1;
    s_jumpStop = 1;
    c->jumpHook = 0;
}

// --stop-on-nops: stops after 1000 consecutive NOPs (running through empty memory).
static void nop_hook(void *user)
{
    H64System *sys = (H64System *)user;
    H64Cpu *c = &sys->cpu;
    int k;
    printf("[nops] 1000 NOPs ending at %08X, instr #%llu; last jumps:", (u32)c->curPc, (unsigned long long)c->instructions);
    for (k = 0; k < 16; k++)
        printf("%s%08X->%08X", (k % 4) ? " " : "\n[nops]   ", c->jumpFrom[(c->jumpPos + k) & 15], c->jumpTo[(c->jumpPos + k) & 15]);
    printf("\n");
    sys->stop = 1;
    s_jumpStop = 1;
    c->nopHook = 0;
}

static u8 *read_file(const char *path, u32 *size)
{
    FILE *f = fopen(path, "rb");
    u8 *data;
    long len;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (u8 *)malloc(len > 0 ? (size_t)len : 1);
    if (data && fread(data, 1, (size_t)len, f) != (size_t)len) { free(data); data = 0; }
    fclose(f);
    *size = (u32)len;
    return data;
}

// --async CYCLES: the Xbox's worker path (graphics tasks busy CYCLES, audio
// 100000), with each job run at once on this thread.
static u32 s_ticket;
static u32 async_start(void *user, void (*job)(void *arg), void *arg) { (void)user; job(arg); return ++s_ticket; }
static void async_wait(void *user) { (void)user; }
static void async_wait_ticket(void *user, u32 ticket) { (void)user; (void)ticket; }
static void async_audio_start(void *user, void (*job)(void *arg), void *arg) { (void)user; job(arg); }

static void print_state(H64System *sys)
{
    H64Cpu *c = &sys->cpu;
    int i;
    printf("pc=%08X%08X instructions=%llu cycles=%llu count=%08X frames=%u\n", (u32)(c->pc >> 32), (u32)c->pc,
           (unsigned long long)c->instructions, (unsigned long long)c->cycles, h64_cpu_count(c), sys->vi.frames);
    for (i = 0; i < 32; i++)
        printf("r%-2d=%08X%08X%s", i, (u32)(c->gpr[i] >> 32), (u32)c->gpr[i], (i % 4) == 3 ? "\n" : "  ");
    printf("status=%08X cause=%08X epc=%08X%08X badvaddr=%08X%08X mi_intr=%02X mi_mask=%02X\n",
           (u32)c->cop0[CP0_STATUS], (u32)c->cop0[CP0_CAUSE], (u32)(c->cop0[CP0_EPC] >> 32), (u32)c->cop0[CP0_EPC],
           (u32)(c->cop0[CP0_BADVADDR] >> 32), (u32)c->cop0[CP0_BADVADDR], sys->mi.intr, sys->mi.mask);
    printf("last jumps:");
    for (i = 0; i < 16; i++)
        printf("%s%08X->%08X", (i % 4) ? " " : "\n  ", c->jumpFrom[(c->jumpPos + i) & 15], c->jumpTo[(c->jumpPos + i) & 15]);
    printf("\n");
}

// ---- Input script: "BUTTON@first[-last],..." in VI frames, e.g. "START@300-305,A@600".
// Buttons: A B Z START L R DU DD DL DR CU CD CL CR, or X=n / Y=n for the stick.


static H64InputScript s_script;

static int parse_input(const char *script) { return h64_input_script_parse(&s_script, script); }

// --null-renderer: the configuration of a GPU renderer without a GPU (the
// software RDP keeps state only; primitives are counted, not drawn).
static u32 s_nullTris;
static void null_rdp(void *user, const u64 *w, u32 n) { h64_rdp_command((H64System *)user, w, n); }
static void null_triangle(void *user, const H64RenderVertex *a, const H64RenderVertex *b, const H64RenderVertex *c,
                          u32 flags, u32 tile, u32 levels)
{
    (void)user; (void)a; (void)b; (void)c; (void)flags; (void)tile; (void)levels;
    s_nullTris++;
}
static H64Renderer s_nullRenderer;

static void apply_input(H64System *sys) { h64_input_script_apply(&s_script, sys); }

// ---- Audio capture to WAV ----
struct WavCapture { u8 *data; u32 size, cap, rate; };

static void wav_sink(void *user, const u8 *samples, u32 len, u32 rate)
{
    WavCapture *w = (WavCapture *)user;
    u32 i;
    if (!w->rate) w->rate = rate;
    if (w->size + len > w->cap)
    {
        u32 cap = w->cap ? w->cap * 2 : 1 << 20;
        while (cap < w->size + len) cap *= 2;
        w->data = (u8 *)realloc(w->data, cap);
        w->cap = cap;
    }
    for (i = 0; i + 1 < len; i += 2)   // big-endian -> little-endian samples
    {
        w->data[w->size + i] = samples[i + 1];
        w->data[w->size + i + 1] = samples[i];
    }
    w->size += len & ~1u;
}

static void put_le32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static int wav_write(const char *path, const WavCapture *w)
{
    u8 h[44];
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    memcpy(h, "RIFF", 4); put_le32(h + 4, 36 + w->size); memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;   // PCM, stereo
    put_le32(h + 24, w->rate); put_le32(h + 28, w->rate * 4); h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); put_le32(h + 40, w->size);
    fwrite(h, 1, 44, f);
    if (w->size) fwrite(w->data, 1, w->size, f);
    fclose(f);
    return 0;
}

static int run_rom(const char *path, int argc, char **argv, int first)
{
    static WavCapture wav;
    static struct { u32 frame; const char *path; int done, raw; } shots[128];
    int shotCount = 0;
    const char *wavPath = 0;
    const char *saveDir = 0;
    const char *loadState = 0, *saveStatePath = 0;
    u32 probeX = 0, probeY = 0, probeFrame = 0, probeCount = 2;
    int probe = 0;
    u32 saveStateFrame = 0;
    int stateSaved = 0;
    char saveDirSep[512];
    H64System *sys;
    u8 *file;
    u32 size;
    u32 watchPc = 0, jumpLimit = 0;
    int logPc = 0, gfxCostLog = 0, gfxTiming = 0, padCount = 1;
    u32 rdpLogFrame = 0;
    u32 watchWord = 0, watchLast = 0, watchReports = 0;
    int stopOnNops = 0;
    const char *fbPng = 0, *rawPng = 0, *dumpRam = 0, *jitDump = 0;
    int useJit = 0, lockstep = 0, noRdp = 0, hleAudio = 0, hleGfx = 0, nullRenderer = 0;
    u32 asyncCycles = 0;
    u32 traceFrames = 0, traceStep = 0;
    int jitOps = 0, noJitFpu = 0, cpi = 1, noLink = 0, noRegCache = 0, noFpCache = 0, fastFpu = 0, fullExits = 0;
    H64System *ref = 0;
    int i, frames = 600, dillon = 0, info = 0, state = 0, result = 0;
    double seconds = 0;
    u64 limit;

    for (i = first; i < argc; i++)
    {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dillon")) dillon = 1;
        else if (!strcmp(argv[i], "--until") && i + 1 < argc) s_untilText = argv[++i];
        else if (!strcmp(argv[i], "--info")) info = 1;
        else if (!strcmp(argv[i], "--state")) state = 1;
        else if (!strcmp(argv[i], "--trace-exc") && i + 1 < argc) s_traceExc = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch-pc") && i + 1 < argc) watchPc = (u32)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--gfx-cost-log")) gfxCostLog = 1;
        else if (!strcmp(argv[i], "--rdp-log") && i + 1 < argc) rdpLogFrame = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gfx-timing")) gfxTiming = 1;
        else if (!strcmp(argv[i], "--log-word") && i + 1 < argc) s_logWord = (u32)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--log-from") && i + 1 < argc) s_logFrom = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch-word") && i + 1 < argc) watchWord = (u32)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--log-pc") && i + 1 < argc)
        {
            char *colon;
            watchPc = (u32)strtoul(argv[++i], &colon, 16);
            if (*colon == ':') s_logPcA0 = (u32)strtoul(colon + 1, 0, 16);
            logPc = 1;
        }
        else if (!strcmp(argv[i], "--jump-limit") && i + 1 < argc) jumpLimit = (u32)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--stop-on-nops")) stopOnNops = 1;
        else if (!strcmp(argv[i], "--fb-png") && i + 1 < argc) fbPng = argv[++i];
        else if (!strcmp(argv[i], "--raw-png") && i + 1 < argc) rawPng = argv[++i];
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc) dumpRam = argv[++i];
        else if (!strcmp(argv[i], "--jit-dump") && i + 1 < argc) jitDump = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wavPath = argv[++i];
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc) saveDir = argv[++i];
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) loadState = argv[++i];
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc)
        {
            // --probe X,Y,FRAME[,COUNT]: log every RDP pixel write at (X, Y) during COUNT VI frames from FRAME (default 2; software RDP).
            if (sscanf(argv[++i], "%u,%u,%u,%u", &probeX, &probeY, &probeFrame, &probeCount) < 3) { fprintf(stderr, "--probe takes X,Y,FRAME\n"); return 2; }
            probe = 1;
        }
        else if (!strcmp(argv[i], "--save-state") && i + 1 < argc)
        {
            char *colon;
            saveStateFrame = (u32)strtoul(argv[++i], &colon, 10);
            if (*colon != ':') { fprintf(stderr, "--save-state takes FRAME:file\n"); return 2; }
            saveStatePath = colon + 1;
        }
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) { if (parse_input(argv[++i])) return 2; }
        else if (!strcmp(argv[i], "--pads") && i + 1 < argc) padCount = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "--shot") || !strcmp(argv[i], "--raw-shot")) && i + 1 < argc && shotCount < 128)
        {
            char *colon;
            shots[shotCount].frame = (u32)strtoul(argv[++i], &colon, 10);
            shots[shotCount].path = *colon == ':' ? colon + 1 : 0;
            shots[shotCount].done = 0;
            shots[shotCount].raw = argv[i - 1][2] == 'r';   // --raw-shot: the framebuffer the VI points at
            if (!shots[shotCount].path) { fprintf(stderr, "--shot takes FRAME:file.png\n"); return 2; }
            shotCount++;
        }
        else if (!strcmp(argv[i], "--verbose")) h64_log_set_level(H64_LOG_DEBUG);
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) { i++; useJit = !strcmp(argv[i], "dynarec"); }
        else if (!strcmp(argv[i], "--lockstep")) lockstep = 1;
        else if (!strcmp(argv[i], "--no-rdp")) noRdp = 1;
        else if (!strcmp(argv[i], "--hle-audio")) hleAudio = 1;
        else if (!strcmp(argv[i], "--hle-audio-check")) hleAudio = 2;
        else if (!strcmp(argv[i], "--hle-gfx")) hleGfx = 1;
        else if (!strcmp(argv[i], "--null-renderer")) nullRenderer = 1;
        else if (!strcmp(argv[i], "--no-fpu-flags")) h64_fenv_disable_host_flags();
        else if (!strcmp(argv[i], "--jit-ops")) jitOps = 1;
        else if (!strcmp(argv[i], "--no-jit-fpu")) noJitFpu = 1;
        else if (!strcmp(argv[i], "--no-link")) noLink = 1;
        else if (!strcmp(argv[i], "--no-regcache")) noRegCache = 1;
        else if (!strcmp(argv[i], "--no-fp-cache")) noFpCache = 1;
        else if (!strcmp(argv[i], "--fast-fpu")) fastFpu = 1;
        else if (!strcmp(argv[i], "--full-exits")) fullExits = 1;
        else if (!strcmp(argv[i], "--cpi") && i + 1 < argc) cpi = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace-frames") && i + 1 < argc) traceFrames = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace-step") && i + 1 < argc) traceStep = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hle")) { hleGfx = 1; hleAudio = 1; }
        else if (!strcmp(argv[i], "--async") && i + 1 < argc) asyncCycles = (u32)atoi(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    file = read_file(path, &size);
    if (!file) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    if (size >= 4 && file[0] == 'P' && file[1] == 'K' && file[2] == 3 && file[3] == 4)
    {
        // A zip: the first N64 ROM inside.
        H64ZipMem zm;
        H64ZipReader zr;
        H64ZipEntry ze;
        u8 *rom;
        zm.data = file;
        zm.size = size;
        zr.user = &zm;
        zr.size = size;
        zr.read = h64_zip_mem_read;
        if (h64_zip_find_rom(&zr, &ze)) { fprintf(stderr, "no N64 ROM in %s\n", path); return 2; }
        rom = (u8 *)malloc(ze.size ? ze.size : 1);
        if (!rom || h64_zip_extract(&zr, &ze, rom, ze.size)) { fprintf(stderr, "cannot extract %s from %s\n", ze.name, path); return 2; }
        printf("[rom] %s: %s (%u bytes)\n", path, ze.name, ze.size);
        free(file);
        file = rom;
        size = ze.size;
    }
    sys =(H64System *)malloc(sizeof(H64System));
    if (!sys || h64_system_init(sys, file, size, 0)) { fprintf(stderr, "not an N64 ROM: %s\n", path); return 2; }
    if (lockstep)
    {
        // The reference system: same ROM, interpreter.
        ref = (H64System *)malloc(sizeof(H64System));
        if (!ref || h64_system_init(ref, file, size, 0)) { fprintf(stderr, "cannot create the reference system\n"); return 2; }
        ref->options.noRdpDraw = noRdp;
        ref->options.hleAudio = hleAudio == 1;
        ref->options.hleGfx = hleGfx;
        ref->padHook = apply_input;
        ref->padMask = padCount >= 1 && padCount <= 4 ? (1u << padCount) - 1u : 1u;
        ref->cpu.cpi = (u32)(cpi > 0 ? cpi : 1);
        useJit = 1;
    }
    sys->cpu.cpi = (u32)(cpi > 0 ? cpi : 1);
    free(file);
    if (info)
    {
        char folder[64];
        h64_save_folder_name(&sys->rom, folder, sizeof(folder));
        printf("[info] save folder name: %s\n", folder);
        h64_system_free(sys);
        free(sys);
        return 0;
    }
    if (saveDir)
    {
        // Save files (eeprom.bin, sram.bin, flash.bin, pak1.bin) read from and written back to that folder.
        size_t n = strlen(saveDir);
        if (n + 2 > sizeof(saveDirSep)) return 2;
        memcpy(saveDirSep, saveDir, n + 1);
        if (n && saveDir[n - 1] != '/' && saveDir[n - 1] != '\\') strcat(saveDirSep, "/");
        h64_save_load(sys->save, saveDirSep);
    }
    if (useJit && enable_jit(sys)) return 2;
    if (jitOps && sys->jit) sys->jit->opHist = (u32 *)calloc(232, sizeof(u32));
    if (noJitFpu && sys->jit) sys->jit->noFpu = 1;
    if (noLink && sys->jit) sys->jit->noLink = 1;
    if (noRegCache && sys->jit) sys->jit->noRegCache = 1;
    if (noFpCache && sys->jit) sys->jit->noFpCache = 1;
    if (fastFpu && sys->jit) { sys->jit->fastFpu = 1; h64_jit_reset(sys); }
    if (fullExits && sys->jit) sys->jit->fullExits = 1;
    if (jitDump && sys->jit) sys->jit->dumpFile = fopen(jitDump, "wb");
    sys->isvSink = isv_sink;
    sys->isvUser = sys;
    sys->cpu.excHook = exc_hook;
    sys->cpu.excUser = sys;
    if (watchPc) { sys->cpu.watchPc = watchPc; sys->cpu.watchHook = logPc ? log_pc_hook : watch_hook; }
    if (jumpLimit) { sys->cpu.jumpLimit = jumpLimit; sys->cpu.jumpHook = jump_hook; }
    s_sysForStop = sys;
    if (stopOnNops) sys->cpu.nopHook = nop_hook;
    if (wavPath) { sys->aiSink = wav_sink; sys->aiUser = &wav; }
    sys->options.noRdpDraw = noRdp;
    sys->options.hleAudio = hleAudio == 1;
    sys->options.hleAudioCheck = hleAudio == 2;
    sys->options.gfxCostLog = gfxCostLog;
    sys->options.rdpLogFrame = rdpLogFrame;
    sys->padMask = padCount >= 1 && padCount <= 4 ? (1u << padCount) - 1u : 1u;   // --pads N: ports 1..N plugged in
    sys->options.gfxTiming = gfxTiming;
    sys->options.hleGfx = hleGfx;
    if (asyncCycles)
    {
        sys->asyncStart = async_start;
        sys->asyncWait = async_wait;
        sys->asyncWaitTicket = async_wait_ticket;
        sys->asyncGfxCycles = asyncCycles;
        sys->asyncAudioStart = async_audio_start;
        sys->asyncAudioWait = async_wait;
        sys->asyncAudioCycles = 100000;
    }
    if (nullRenderer)
    {
        s_nullRenderer.user = sys;
        s_nullRenderer.rdp = null_rdp;
        s_nullRenderer.triangle = null_triangle;
        sys->renderer = &s_nullRenderer;
        sys->options.rdpStateOnly = 1;
    }
    sys->padHook = apply_input;

    limit = seconds > 0 ? (u64)(seconds * 93750000.0) : (u64)frames * sys->vi.frameCycles;
    if (loadState)
    {
        u32 n;
        u8 *st = read_file(loadState, &n);
        if (!st || h64_state_load(sys, st, n)) { fprintf(stderr, "cannot load the state %s\n", loadState); return 2; }
        if (ref && h64_state_load(ref, st, n)) { fprintf(stderr, "cannot load the state into the reference system\n"); return 2; }   // --lockstep
        free(st);
    }
    if (traceFrames && traceStep >= 1000000000u)
    {
        // Instruction trace: --trace-step 1000000000+START runs to cycle START,
        // then logs --trace-frames instructions (pc, CPU hash).
        u32 n;
        h64_system_run_cycles(sys, traceStep - 1000000000u);
        for (n = 0; n < traceFrames; n++)
        {
            u32 ch, pc = (u32)sys->cpu.pc;
            h64_system_step(sys);
            h64_system_state_hash(sys, &ch, NULL);
            printf("[itrace] %u pc=%08X cyc=%llu cpu=%08X\n", n, pc, (unsigned long long)sys->cpu.cycles, ch);
        }
        return 0;
    }
    if (traceFrames)
    {
        // Same lines as the Xbox build's trace= option, to find where two hosts diverge.
        u32 n;
        u64 step = traceStep ? traceStep : sys->vi.frameCycles;
        for (n = 0; n < traceFrames; n++)
        {
            u32 ch, rh;
            h64_system_run_cycles(sys, step);
            h64_system_state_hash(sys, &ch, &rh);
            printf("[trace] %u cycles=%llu instr=%llu pc=%08X cpu=%08X ram=%08X\n", n, (unsigned long long)sys->cpu.cycles,
                   (unsigned long long)sys->cpu.instructions, (u32)sys->cpu.pc, ch, rh);
        }
        return 0;
    }
    if (lockstep)
    {
        H64LockstepResult ls;
        int bad = h64_lockstep_run(ref, sys, limit, &ls);
        if (bad)
            printf("LOCKSTEP DIVERGENCE after dispatch %llu at pc %08X (cycle %llu, frame %u): %s\n", (unsigned long long)ls.dispatches,
                   ls.pc, (unsigned long long)ls.cyclesBefore, ls.frame, ls.why);
        printf("[lockstep] %s: %llu dispatches, %llu blocks run, %llu interpreter steps, %llu blocks compiled, %llu invalidations, %llu cache flushes, %llu idle instructions skipped\n",
               bad ? "DIVERGED" : "OK, no divergence", (unsigned long long)ls.dispatches, (unsigned long long)sys->jit->stats.blocksRun,
               (unsigned long long)sys->jit->stats.interpSteps, (unsigned long long)sys->jit->stats.blocksCompiled,
               (unsigned long long)sys->jit->stats.invalidations, (unsigned long long)sys->jit->stats.flushes,
               (unsigned long long)sys->jit->stats.idleSkipped);
        if (bad) result = 1;
        h64_system_free(ref);
        free(ref);
        ref = 0;
    }
    while (sys->cpu.cycles < limit && !s_untilHit && !sys->exitRequested && !s_jumpStop)
    {
        {
            int k;
            for (k = 0; k < shotCount; k++)
                if (!shots[k].done && sys->vi.frames >= shots[k].frame)
                {
                    static u8 img[1024 * 1024 * 3];
                    int sw, sh;
                    shots[k].done = 1;
                    if ((shots[k].raw ? h64_vi_capture(sys, img, 1024, 1024, &sw, &sh) : h64_vi_render(sys, img, 1024, 1024, &sw, &sh)) == 0 &&
                        h64_png_write_rgb(shots[k].path, img, sw, sh) == 0)
                        printf("[shot] frame %u: %s\n", sys->vi.frames, shots[k].path);
                }
        }
        if (probe)
        {
            H64RdpState *ps = h64_rdp_state(sys);
            ps->probeOn = sys->vi.frames >= probeFrame && sys->vi.frames < probeFrame + probeCount;
            ps->probeX = probeX;
            ps->probeY = probeY;
        }
        if (saveStatePath && !stateSaved && sys->vi.frames >= saveStateFrame && h64_state_quiet(sys))
        {
            // At the first quiet point from that frame on (between two run slices).
            std::vector<u8> st;
            FILE *f;
            h64_state_save(sys, &st);
            f = fopen(saveStatePath, "wb");
            if (!f || fwrite(&st[0], 1, st.size(), f) != st.size()) { fprintf(stderr, "cannot write %s\n", saveStatePath); result = 1; }
            if (f) fclose(f);
            stateSaved = 1;
        }
        h64_system_run_cycles(sys, dillon ? 10000 : watchWord ? (sys->vi.frames >= 284 ? 2 : 200) : 1000000);
        if (watchWord)
        {
            // --watch-word: report every change of that RDRAM word (200-cycle steps).
            u32 v = h64_load_be32(sys->rdram + (watchWord & 0x7FFFFC));
            if (v != watchLast && watchReports < 200)
            {
                watchReports++;
                printf("[watchword] %08X: %08X -> %08X at pc %08X #%llu frame %u, %u RSP tasks (%u HLE)\n", watchWord, watchLast, v,
                       (u32)sys->cpu.pc, (unsigned long long)sys->cpu.instructions, sys->vi.frames, sys->rsp.tasks, sys->rsp.hleTasks);
            }
            watchLast = v;
        }
        if (dillon && sys->cpu.gpr[30] != 0)
            break;
    }

    if (dillon)
    {
        s64 r30 = (s64)sys->cpu.gpr[30];
        if (r30 == -1) printf("DILLON PASS\n");
        else if (r30 == 0) { printf("DILLON TIMEOUT\n"); result = 1; }
        else { printf("DILLON FAIL test %lld\n", (long long)r30); result = 1; }
    }
    if (s_untilText && !s_untilHit) { printf("UNTIL not reached: \"%s\"\n", s_untilText); result = 1; }
    if (sys->exitRequested) printf("[run] the ROM requested exit (EMUX)\n");
    printf("[run] %u frames, %.2f emulated s, %llu instructions, %u RSP tasks (%u HLE), %llu RSP instructions, %llu RDP commands\n",
           sys->vi.frames, (double)sys->cpu.cycles / 93750000.0, (unsigned long long)sys->cpu.instructions,
           sys->rsp.tasks, sys->rsp.hleTasks, (unsigned long long)sys->rsp.instructions, (unsigned long long)sys->dpCommands);
    {
        u32 ch, rh;
        h64_system_state_hash(sys, &ch, &rh);
        printf("[run] state hash cpu=%08X ram=%08X\n", ch, rh);
        if (sys->ai.statBuffers)
            printf("[run] AI: %u buffers, %.1f samples each; %llu AI_LEN reads, %.1f samples left on average\n", sys->ai.statBuffers,
                   sys->ai.statQueued / 4.0 / sys->ai.statBuffers, (unsigned long long)sys->ai.statReads,
                   sys->ai.statReads ? sys->ai.statReadSum / 4.0 / sys->ai.statReads : 0.0);
    }
    if (sys->jit)
        printf("[jit] %llu instructions executed, %llu skipped in idle loops, %llu blocks run, %llu interpreted\n",
               (unsigned long long)(sys->cpu.instructions - sys->jit->stats.idleSkipped),
               (unsigned long long)sys->jit->stats.idleSkipped, (unsigned long long)sys->jit->stats.blocksRun,
               (unsigned long long)sys->jit->stats.helperCalls);
    if (sys->jit)
        printf("[jit] code: %.1f bytes per instruction (%.1f before the slow paths), %u bytes in use for %u live blocks, %llu instructions compiled (%llu native), %llu flushes\n",
               (double)sys->jit->stats.codeBytes / (double)(sys->jit->stats.nativeInsns + sys->jit->stats.helperInsns + 1),
               (double)sys->jit->stats.hotBytes / (double)(sys->jit->stats.nativeInsns + sys->jit->stats.helperInsns + 1),
               sys->jit->memUsed, sys->jit->blockCount,
               (unsigned long long)(sys->jit->stats.nativeInsns + sys->jit->stats.helperInsns),
               (unsigned long long)sys->jit->stats.nativeInsns, (unsigned long long)sys->jit->stats.flushes);
    {
        int k;
        printf("[run] exceptions:");
        for (k = 0; k < 32; k++) if (s_excCount[k]) printf(" code %d: %llu", k, s_excCount[k]);
        printf("\n");
    }
    if (sys->jit)
        printf("[jit] links: %u of %u records used, %llu made; TLB checks failed %llu, remapped blocks unlinked %llu\n",
               sys->jit->linkCount, sys->jit->linkCap, (unsigned long long)sys->jit->stats.linksMade,
               (unsigned long long)sys->jit->stats.tlbStale, (unsigned long long)sys->jit->stats.tlbUnlinks);
    if (sys->jit && sys->jit->opHist)
    {
        // The most frequent instructions that ran through the interpreter helper.
        static const char *cls[4] = { "op", "special", "cop1", "cop1 rs" };
        printf("[jit-ops] memory slow path: not KSEG0/1 %u, unaligned %u, outside RDRAM %u, store to a code page %u, other %u\n",
               sys->jit->opHist[192], sys->jit->opHist[193], sys->jit->opHist[194], sys->jit->opHist[195],
               sys->jit->opHist[196]);
        u32 k, n;
        for (n = 0; n < 15; n++)
        {
            u32 best = 0, bi = 0;
            for (k = 0; k < 232; k++)
                if ((k < 192 || k >= 200) && sys->jit->opHist[k] > best) { best = sys->jit->opHist[k]; bi = k; }
            if (!best) break;
            if (bi >= 216) printf("[jit-ops] cvt.%c from W/L: %u\n", bi == 216 ? 's' : 'd', best);
            else if (bi >= 200) printf("[jit-ops] %-7s %02X: %u\n", cls[3], bi - 200, best);
            else printf("[jit-ops] %-7s %02X: %u\n", cls[bi / 64], bi % 64, best);
            sys->jit->opHist[bi] = 0;
        }
    }
    if (nullRenderer) printf("[run] null renderer: %u triangles\n", s_nullTris);
    printf("[run] interrupts raised: SP %u, SI %u, AI %u, VI %u, PI %u, DP %u\n", sys->miRaised[0], sys->miRaised[1],
           sys->miRaised[2], sys->miRaised[3], sys->miRaised[4], sys->miRaised[5]);
    printf("[run] VI control %08X, width %u, x scale %08X, y scale %08X\n", sys->vi.regs[0], sys->vi.regs[2],
           sys->vi.regs[12], sys->vi.regs[13]);
    if (state) print_state(sys);
    if (dumpRam)
    {
        FILE *f = fopen(dumpRam, "wb");   // RDRAM then SP DMEM/IMEM, at the end of the run
        if (f) { fwrite(sys->rdram, 1, H64_RDRAM_SIZE, f); fwrite(sys->spMem, 1, 0x2000, f); fclose(f); }
    }
    if (fbPng || rawPng)
    {
        static u8 rgb[1024 * 1024 * 3];
        int w = 0, h = 0, k;
        for (k = 0; k < 2; k++)
        {
            const char *out = k == 0 ? fbPng : rawPng;
            int ok;
            if (!out) continue;
            ok = k == 0 ? h64_vi_render(sys, rgb, 1024, 1024, &w, &h) : h64_vi_capture(sys, rgb, 1024, 1024, &w, &h);
            if (ok == 0 && h64_png_write_rgb(out, rgb, w, h) == 0)
                printf("[run] %s %dx%d written to %s\n", k == 0 ? "VI output" : "framebuffer", w, h, out);
            else
                printf("[run] no %s (VI blank)\n", k == 0 ? "VI output" : "framebuffer");
        }
    }
    if (wavPath)
    {
        if (wav_write(wavPath, &wav) == 0)
            printf("[run] audio: %u samples at %u Hz written to %s\n", wav.size / 4, wav.rate, wavPath);
        free(wav.data);
        memset(&wav, 0, sizeof(wav));
    }
    if (saveDir && h64_save_store(sys->save, saveDirSep) < 0) result = result ? result : 1;
    h64_system_free(sys);
    free(sys);
    return result;
}

static void usage(void)
{
    printf("usage: h64test --version | --unit | --rom FILE [--frames N] [--seconds S] [--dillon]\n"
           "               [--until TEXT] [--info] [--state] [--verbose]\n");
}

int main(int argc, char **argv)
{
    int i;
    h64_log_set_sink(stdout_sink);
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--verbose")) h64_log_set_level(H64_LOG_DEBUG);
    if (argc < 2) { usage(); return 2; }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "--unit"))
    {
        printf("Harissa64 V2 %s, %s-endian host, %d-bit pointers\n", H64_VERSION_STRING,
               H64_HOST_BIG_ENDIAN ? "big" : "little", (int)(sizeof(void *) * 8));
        if (!strcmp(argv[1], "--unit"))
        {
            int tests = 0, checks = 0;
            int failures = h64_run_all_unit_tests(&tests, &checks);
            printf("UNIT %s: %d tests, %d checks, %d failures\n", failures ? "FAILED" : "PASSED", tests, checks,
                   failures);
            return failures ? 1 : 0;
        }
        return 0;
    }
    // --rom FILE may come anywhere among the options.
    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--rom"))
        {
            static char *rest[256];
            int n = 1, k;
            const char *path = argv[i + 1];
            rest[0] = argv[0];
            for (k = 1; k < argc && n < 255; k++)
                if (k != i && k != i + 1) rest[n++] = argv[k];
            return run_rom(path, n, rest, 1);
        }
    usage();
    return 2;
}
