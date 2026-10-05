// h64test - Harissa64 V2 headless runner.
//
// M0: --version and --unit (runs the unit tests). Later milestones add ROM
// execution, state dumps, screenshots and audio dumps.
#include <stdio.h>
#include <string.h>

#include "../../core/common/h64_types.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../tests/unit/unit_tests.h"

static void stdout_sink(int level, const char *line)
{
    FILE *f = level <= H64_LOG_WARN ? stderr : stdout;
    fprintf(f, "%s\n", line);
}

static void usage(void)
{
    printf("usage: h64test [--version] [--unit] [--verbose]\n"
           "  --version   print version, byte order and pointer size\n"
           "  --unit      run the unit tests (exit code 1 on failure)\n");
}

int main(int argc, char **argv)
{
    int i, runUnit = 0, showVersion = 0;
    h64_log_set_sink(stdout_sink);
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--unit")) runUnit = 1;
        else if (!strcmp(argv[i], "--version")) showVersion = 1;
        else if (!strcmp(argv[i], "--verbose")) h64_log_set_level(H64_LOG_DEBUG);
        else { usage(); return 2; }
    }
    if (!runUnit && !showVersion)
    {
        usage();
        return 2;
    }
    if (showVersion || runUnit)
        printf("Harissa64 V2 %s, %s-endian host, %d-bit pointers\n", H64_VERSION_STRING,
               H64_HOST_BIG_ENDIAN ? "big" : "little", (int)(sizeof(void *) * 8));
    if (runUnit)
    {
        int tests = 0, checks = 0;
        int failures = h64_run_all_unit_tests(&tests, &checks);
        printf("UNIT %s: %d tests, %d checks, %d failures\n", failures ? "FAILED" : "PASSED", tests, checks, failures);
        return failures ? 1 : 0;
    }
    return 0;
}
