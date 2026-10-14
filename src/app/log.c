#include "app/log.h"

#include <stdarg.h>
#include <stdio.h>

void SosLog(SosLogLevel level, const char *format, ...)
{
    static const char *const levels[] = {"info", "warning", "error"};
    const char *label = (unsigned)level < sizeof(levels) / sizeof(levels[0]) ? levels[level] : "?";
    fprintf(stderr, "sos %s: ", label);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    fflush(stderr);
}
