#include "sim/calendar.h"

/* Shifting by the start time keeps tick 0 at dawn; every quantity is a
 * non-negative integer, so the divisions below are plain floors. */
static uint64_t SinceMidnightOfDayOne(uint64_t tick)
{
    return tick + (uint64_t)SOS_START_TICK_OF_DAY;
}

int64_t SosCalendarTickOfDay(uint64_t tick)
{
    return (int64_t)(SinceMidnightOfDayOne(tick) % (uint64_t)SOS_TICKS_PER_DAY);
}

SosDate SosCalendarDate(uint64_t tick)
{
    const uint64_t total = SinceMidnightOfDayOne(tick);
    const uint64_t days = total / (uint64_t)SOS_TICKS_PER_DAY;
    const int64_t tickOfDay = (int64_t)(total % (uint64_t)SOS_TICKS_PER_DAY);
    return (SosDate){
        .year = (int64_t)(days / (uint64_t)SOS_DAYS_PER_YEAR) + 1,
        .day = (int32_t)(days % (uint64_t)SOS_DAYS_PER_YEAR) + 1,
        .hour = (int32_t)(tickOfDay / SOS_TICKS_PER_HOUR),
        .minute = (int32_t)(tickOfDay % SOS_TICKS_PER_HOUR / SOS_TICKS_PER_MINUTE),
    };
}

bool SosCalendarIsNight(uint64_t tick)
{
    const int64_t tickOfDay = SosCalendarTickOfDay(tick);
    return tickOfDay >= 22 * SOS_TICKS_PER_HOUR || tickOfDay < 6 * SOS_TICKS_PER_HOUR;
}
