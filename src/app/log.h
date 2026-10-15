#ifndef SOS_APP_LOG_H
#define SOS_APP_LOG_H

typedef enum SosLogLevel
{
    SOS_LOG_INFO = 0,
    SOS_LOG_WARNING,
    SOS_LOG_ERROR,
} SosLogLevel;

/* One line to standard error, or to logcat on Android, prefixed with the game
 * name and the level. The format follows printf; the line break is added
 * here. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void SosLog(SosLogLevel level, const char *format, ...);

#endif
