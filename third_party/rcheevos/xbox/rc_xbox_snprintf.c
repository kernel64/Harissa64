/* Harissa64: snprintf for rcheevos on the Xbox 360 SDK's compiler (Visual
 * Studio 2010 has only _snprintf, which does not end a truncated text).
 * The project defines snprintf=rc_xbox_snprintf for the rcheevos files. */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

int rc_xbox_snprintf(char *buffer, size_t size, const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = _vsnprintf(buffer, size, format, args);
    va_end(args);
    if (size && (result < 0 || (size_t)result >= size))
    {
        buffer[size - 1] = 0;
        result = (int)size - 1;
    }
    return result;
}
