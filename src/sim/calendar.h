#ifndef SOS_SIM_CALENDAR_H
#define SOS_SIM_CALENDAR_H

#include "sim/clock.h"

#include <stdbool.h>
#include <stdint.h>

/* Game time. A tick is 1/30 second, a day 24 hours, a year 365 days; leap
 * days and calendars of future cultures are presentation. The world begins at
 * dawn, 06:00 on day 1 of year 1. */

#define SOS_TICKS_PER_SECOND ((int64_t)SOS_CLOCK_TICKS_PER_SECOND)
#define SOS_TICKS_PER_MINUTE (60 * SOS_TICKS_PER_SECOND)
#define SOS_TICKS_PER_HOUR (60 * SOS_TICKS_PER_MINUTE)
#define SOS_TICKS_PER_DAY (24 * SOS_TICKS_PER_HOUR)
#define SOS_DAYS_PER_YEAR INT64_C(365)
#define SOS_TICKS_PER_YEAR (SOS_DAYS_PER_YEAR * SOS_TICKS_PER_DAY)
#define SOS_START_TICK_OF_DAY (6 * SOS_TICKS_PER_HOUR)

typedef struct SosDate
{
    int64_t year;   /* 1-based */
    int32_t day;    /* 1..365 */
    int32_t hour;   /* 0..23 */
    int32_t minute; /* 0..59 */
} SosDate;

SosDate SosCalendarDate(uint64_t tick);
/* Tick within the day, counted from midnight. */
int64_t SosCalendarTickOfDay(uint64_t tick);
/* Night lasts from 22:00 to 06:00. */
bool SosCalendarIsNight(uint64_t tick);

#endif
