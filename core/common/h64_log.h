// Harissa64 V2 - logging.
//
// The core never prints directly: it formats a line and hands it to a sink
// installed by the platform layer (stdout for h64test, a log file on the
// console). Without a sink, lines are dropped.
#ifndef H64_LOG_H
#define H64_LOG_H

#include "h64_types.h"

enum H64LogLevel
{
    H64_LOG_ERROR = 0,
    H64_LOG_WARN = 1,
    H64_LOG_INFO = 2,
    H64_LOG_DEBUG = 3
};

typedef void (*H64LogSink)(int level, const char *line);

void h64_log_set_sink(H64LogSink sink);
void h64_log_set_level(int maxLevel);
void h64_log(int level, const char *fmt, ...);

#define H64_ERROR(...) h64_log(H64_LOG_ERROR, __VA_ARGS__)
#define H64_WARN(...)  h64_log(H64_LOG_WARN, __VA_ARGS__)
#define H64_INFO(...)  h64_log(H64_LOG_INFO, __VA_ARGS__)
#define H64_DEBUG(...) h64_log(H64_LOG_DEBUG, __VA_ARGS__)

#endif
