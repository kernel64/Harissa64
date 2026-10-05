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
//   --verbose             debug log level
// Exit code: 0 normally, 1 for a failed unit test or Dillonb test, 2 for bad usage.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_types.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../core/system/h64_system.h"
#include "../../tests/unit/unit_tests.h"

static void stdout_sink(int level, const char *line)
{
    FILE *f = level <= H64_LOG_WARN ? stderr : stdout;
    fprintf(f, "%s\n", line);
    fflush(f);
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
    if (code == EXC_INT || s_traceExc <= 0) return;
    {
        u32 p, op = 0;
        if (h64_cpu_translate_debug(c, c->curPc, &p)) h64_bus_read32(sys, p, &op);
        printf("[exc] opcode %08X\n", op);
    }
    s_traceExc--;
    printf("[exc] code %d at pc %08X%s, badvaddr %08X%08X, ra %08X, sr %08X, instr #%llu\n", code, (u32)c->curPc,
           c->curInDelaySlot ? " (delay slot)" : "", (u32)(c->cop0[CP0_BADVADDR] >> 32), (u32)c->cop0[CP0_BADVADDR],
           (u32)c->gpr[31], (u32)c->cop0[CP0_STATUS], (unsigned long long)c->instructions);
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
}

static int run_rom(const char *path, int argc, char **argv, int first)
{
    H64System *sys;
    u8 *file;
    u32 size;
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
        else if (!strcmp(argv[i], "--verbose")) h64_log_set_level(H64_LOG_DEBUG);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    file = read_file(path, &size);
    if (!file) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    sys = (H64System *)malloc(sizeof(H64System));
    if (!sys || h64_system_init(sys, file, size, 0)) { fprintf(stderr, "not an N64 ROM: %s\n", path); return 2; }
    free(file);
    if (info) { h64_system_free(sys); free(sys); return 0; }
    sys->isvSink = isv_sink;
    sys->isvUser = sys;
    sys->cpu.excHook = exc_hook;
    sys->cpu.excUser = sys;

    limit = seconds > 0 ? (u64)(seconds * 93750000.0) : (u64)frames * sys->vi.frameCycles;
    while (sys->cpu.cycles < limit && !s_untilHit && !sys->exitRequested)
    {
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
    printf("[run] %u frames, %.2f emulated s, %llu instructions, %u RSP tasks\n", sys->vi.frames,
           (double)sys->cpu.cycles / 93750000.0, (unsigned long long)sys->cpu.instructions, sys->sp.tasks);
    if (state) print_state(sys);
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
    if (!strcmp(argv[1], "--rom") && argc >= 3)
        return run_rom(argv[2], argc, argv, 3);
    usage();
    return 2;
}
