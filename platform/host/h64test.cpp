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
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../core/system/h64_system.h"
#include "../../core/vi/h64_vi.h"
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
    const u32 size = 32u << 20;
    void *mem = exec_alloc(size);
    if (!mem || h64_jit_init(sys, mem, size, flush_icache))
    {
        fprintf(stderr, "the recompiler needs a PowerPC host (ppc64 big-endian Linux or the Xbox 360)\n");
        return -1;
    }
    return 0;
}

// ---- Lockstep: the recompiler (b) against the interpreter (a) ----
static int compare_cpu(const H64System *a, const H64System *b, char *why, size_t whyLen)
{
    const H64Cpu *x = &a->cpu, *y = &b->cpu;
    int i;
    for (i = 0; i < 32; i++)
        if (x->gpr[i] != y->gpr[i]) { snprintf(why, whyLen, "gpr[%d] interp %016llX jit %016llX", i, (unsigned long long)x->gpr[i], (unsigned long long)y->gpr[i]); return 1; }
    for (i = 0; i < 32; i++)
        if (x->fgr[i] != y->fgr[i]) { snprintf(why, whyLen, "fgr[%d] interp %016llX jit %016llX", i, (unsigned long long)x->fgr[i], (unsigned long long)y->fgr[i]); return 1; }
    for (i = 0; i < 32; i++)
        if (i != CP0_RANDOM && x->cop0[i] != y->cop0[i]) { snprintf(why, whyLen, "cop0[%d] interp %016llX jit %016llX", i, (unsigned long long)x->cop0[i], (unsigned long long)y->cop0[i]); return 1; }
    if (x->hi != y->hi || x->lo != y->lo) { snprintf(why, whyLen, "hi/lo"); return 1; }
    if (x->pc != y->pc || x->nextPc != y->nextPc || x->branchPending != y->branchPending)
    {
        snprintf(why, whyLen, "pc interp %016llX/%016llX/%d jit %016llX/%016llX/%d", (unsigned long long)x->pc,
                 (unsigned long long)x->nextPc, x->branchPending, (unsigned long long)y->pc, (unsigned long long)y->nextPc, y->branchPending);
        return 1;
    }
    if (x->fcr31 != y->fcr31 || x->llbit != y->llbit) { snprintf(why, whyLen, "fcr31/llbit"); return 1; }
    if (x->instructions != y->instructions) { snprintf(why, whyLen, "instruction count interp %llu jit %llu", (unsigned long long)x->instructions, (unsigned long long)y->instructions); return 1; }
    return 0;
}

// Runs b (recompiler) one dispatch at a time and a (interpreter) up to the
// same cycle; compares the CPU after each dispatch and RDRAM now and then.
static int run_lockstep(H64System *a, H64System *b, u64 limit, u64 *dispatches)
{
    char why[256];
    u64 n = 0;
    while (b->cpu.cycles < limit && !a->exitRequested && !b->exitRequested)
    {
        u64 before = b->cpu.cycles;
        u32 pc = (u32)b->cpu.pc;
        h64_jit_run_one(b);
        while (a->cpu.cycles < b->cpu.cycles) h64_system_step(a);
        n++;
        if (a->cpu.cycles != b->cpu.cycles || compare_cpu(a, b, why, sizeof(why)) ||
            ((n & 0x3FFF) == 0 && memcmp(a->rdram, b->rdram, H64_RDRAM_SIZE) && snprintf(why, sizeof(why), "RDRAM differs")))
        {
            if (a->cpu.cycles != b->cpu.cycles) snprintf(why, sizeof(why), "cycles interp %llu jit %llu", (unsigned long long)a->cpu.cycles, (unsigned long long)b->cpu.cycles);
            printf("LOCKSTEP DIVERGENCE after dispatch %llu at pc %08X (cycles %llu..%llu, frame %u): %s\n",
                   (unsigned long long)n, pc, (unsigned long long)before, (unsigned long long)b->cpu.cycles, b->vi.frames, why);
            *dispatches = n;
            return 1;
        }
    }
    if (memcmp(a->rdram, b->rdram, H64_RDRAM_SIZE)) { printf("LOCKSTEP DIVERGENCE at the end: RDRAM differs\n"); return 1; }
    *dispatches = n;
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
static void exc_hook(void *user, int code)
{
    H64System *sys = (H64System *)user;
    H64Cpu *c = &sys->cpu;
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
struct InputEvent { u32 first, last; u16 buttons; int axis; int value; };
static InputEvent s_input[64];
static int s_inputCount;

static int parse_input(const char *script)
{
    static const struct { const char *name; u16 bit; } names[] = {
        { "A", 0x8000 }, { "B", 0x4000 }, { "Z", 0x2000 }, { "START", 0x1000 }, { "DU", 0x0800 }, { "DD", 0x0400 },
        { "DL", 0x0200 }, { "DR", 0x0100 }, { "L", 0x0020 }, { "R", 0x0010 }, { "CU", 0x0008 }, { "CD", 0x0004 },
        { "CL", 0x0002 }, { "CR", 0x0001 } };
    const char *p = script;
    while (*p && s_inputCount < 64)
    {
        InputEvent *e = &s_input[s_inputCount];
        char name[16];
        int n = 0;
        unsigned i;
        memset(e, 0, sizeof(*e));
        while (*p && *p != '@' && *p != '=' && n < 15) name[n++] = *p++;
        name[n] = 0;
        if (*p == '=') { e->axis = name[0] == 'X' ? 1 : 2; e->value = (int)strtol(p + 1, (char **)&p, 10); }
        else
            for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                if (!strcmp(name, names[i].name)) e->buttons = names[i].bit;
        if (!e->buttons && !e->axis) { fprintf(stderr, "bad input event \"%s\"\n", name); return -1; }
        if (*p != '@') { fprintf(stderr, "input event \"%s\" needs @frame\n", name); return -1; }
        e->first = e->last = (u32)strtoul(p + 1, (char **)&p, 10);
        if (*p == '-') e->last = (u32)strtoul(p + 1, (char **)&p, 10);
        if (*p == ',') p++;
        s_inputCount++;
    }
    return 0;
}

static void apply_input(H64System *sys)
{
    int i;
    sys->pad[0].buttons = 0;
    sys->pad[0].x = sys->pad[0].y = 0;
    for (i = 0; i < s_inputCount; i++)
        if (sys->vi.frames >= s_input[i].first && sys->vi.frames <= s_input[i].last)
        {
            sys->pad[0].buttons |= s_input[i].buttons;
            if (s_input[i].axis == 1) sys->pad[0].x = (s8)s_input[i].value;
            if (s_input[i].axis == 2) sys->pad[0].y = (s8)s_input[i].value;
        }
}

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
    static struct { u32 frame; const char *path; int done; } shots[32];
    int shotCount = 0;
    const char *wavPath = 0;
    H64System *sys;
    u8 *file;
    u32 size;
    u32 watchPc = 0, jumpLimit = 0;
    int stopOnNops = 0;
    const char *fbPng = 0, *rawPng = 0;
    int useJit = 0, lockstep = 0;
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
        else if (!strcmp(argv[i], "--jump-limit") && i + 1 < argc) jumpLimit = (u32)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--stop-on-nops")) stopOnNops = 1;
        else if (!strcmp(argv[i], "--fb-png") && i + 1 < argc) fbPng = argv[++i];
        else if (!strcmp(argv[i], "--raw-png") && i + 1 < argc) rawPng = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wavPath = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) { if (parse_input(argv[++i])) return 2; }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc && shotCount < 32)
        {
            char *colon;
            shots[shotCount].frame = (u32)strtoul(argv[++i], &colon, 10);
            shots[shotCount].path = *colon == ':' ? colon + 1 : 0;
            shots[shotCount].done = 0;
            if (!shots[shotCount].path) { fprintf(stderr, "--shot takes FRAME:file.png\n"); return 2; }
            shotCount++;
        }
        else if (!strcmp(argv[i], "--verbose")) h64_log_set_level(H64_LOG_DEBUG);
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) { i++; useJit = !strcmp(argv[i], "dynarec"); }
        else if (!strcmp(argv[i], "--lockstep")) lockstep = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    file = read_file(path, &size);
    if (!file) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    sys = (H64System *)malloc(sizeof(H64System));
    if (!sys || h64_system_init(sys, file, size, 0)) { fprintf(stderr, "not an N64 ROM: %s\n", path); return 2; }
    if (lockstep)
    {
        // The reference system: same ROM, interpreter.
        ref = (H64System *)malloc(sizeof(H64System));
        if (!ref || h64_system_init(ref, file, size, 0)) { fprintf(stderr, "cannot create the reference system\n"); return 2; }
        useJit = 1;
    }
    free(file);
    if (info) { h64_system_free(sys); free(sys); return 0; }
    if (useJit && enable_jit(sys)) return 2;
    sys->isvSink = isv_sink;
    sys->isvUser = sys;
    sys->cpu.excHook = exc_hook;
    sys->cpu.excUser = sys;
    if (watchPc) { sys->cpu.watchPc = watchPc; sys->cpu.watchHook = watch_hook; }
    if (jumpLimit) { sys->cpu.jumpLimit = jumpLimit; sys->cpu.jumpHook = jump_hook; }
    s_sysForStop = sys;
    if (stopOnNops) sys->cpu.nopHook = nop_hook;
    if (wavPath) { sys->aiSink = wav_sink; sys->aiUser = &wav; }

    limit = seconds > 0 ? (u64)(seconds * 93750000.0) : (u64)frames * sys->vi.frameCycles;
    if (lockstep)
    {
        u64 dispatches = 0;
        int bad = run_lockstep(ref, sys, limit, &dispatches);
        printf("[lockstep] %s: %llu dispatches, %llu blocks run, %llu interpreter steps, %llu blocks compiled, %llu invalidations, %llu cache flushes\n",
               bad ? "DIVERGED" : "OK, no divergence", (unsigned long long)dispatches, (unsigned long long)sys->jit->stats.blocksRun,
               (unsigned long long)sys->jit->stats.interpSteps, (unsigned long long)sys->jit->stats.blocksCompiled,
               (unsigned long long)sys->jit->stats.invalidations, (unsigned long long)sys->jit->stats.flushes);
        if (bad) result = 1;
        h64_system_free(ref);
        free(ref);
        ref = 0;
    }
    while (sys->cpu.cycles < limit && !s_untilHit && !sys->exitRequested && !s_jumpStop)
    {
        apply_input(sys);
        {
            int k;
            for (k = 0; k < shotCount; k++)
                if (!shots[k].done && sys->vi.frames >= shots[k].frame)
                {
                    static u8 img[1024 * 1024 * 3];
                    int sw, sh;
                    shots[k].done = 1;
                    if (h64_vi_render(sys, img, 1024, 1024, &sw, &sh) == 0 && h64_png_write_rgb(shots[k].path, img, sw, sh) == 0)
                        printf("[shot] frame %u: %s\n", sys->vi.frames, shots[k].path);
                }
        }
        h64_system_run_cycles(sys, dillon ? 10000 : 1000000);
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
    printf("[run] %u frames, %.2f emulated s, %llu instructions, %u RSP tasks, %llu RSP instructions, %llu RDP commands\n",
           sys->vi.frames, (double)sys->cpu.cycles / 93750000.0, (unsigned long long)sys->cpu.instructions,
           sys->rsp.tasks, (unsigned long long)sys->rsp.instructions, (unsigned long long)sys->dpCommands);
    printf("[run] interrupts raised: SP %u, SI %u, AI %u, VI %u, PI %u, DP %u\n", sys->miRaised[0], sys->miRaised[1],
           sys->miRaised[2], sys->miRaised[3], sys->miRaised[4], sys->miRaised[5]);
    if (state) print_state(sys);
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
