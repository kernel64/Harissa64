#include "h64_log.h"

#include <stdarg.h>
#include <stdio.h>

static H64LogSink s_sink = 0;
static int s_maxLevel = H64_LOG_INFO;

void h64_log_set_sink(H64LogSink sink) { s_sink = sink; }
void h64_log_set_level(int maxLevel) { s_maxLevel = maxLevel; }

void h64_log(int level, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    if (!s_sink || level > s_maxLevel)
        return;
    va_start(ap, fmt);
#if defined(_MSC_VER) && _MSC_VER < 1900
    _vsnprintf(line, sizeof(line) - 1, fmt, ap);   // VS2010: no C99 vsnprintf
#else
    vsnprintf(line, sizeof(line) - 1, fmt, ap);
#endif
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    s_sink(level, line);
}
