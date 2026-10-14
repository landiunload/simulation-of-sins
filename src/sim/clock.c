#include "sim/clock.h"

#include <math.h>
#include <stddef.h>

bool SosClockInit(SosClock *clock, uint32_t ticksPerSecond, uint32_t maxTicksPerAdvance)
{
    if (clock == NULL || ticksPerSecond == 0u || ticksPerSecond > SOS_CLOCK_MAX_TICKS_PER_SECOND ||
        maxTicksPerAdvance == 0u)
    {
        return false;
    }
    *clock = (SosClock){
        .ticksPerSecond = ticksPerSecond,
        .maxTicksPerAdvance = maxTicksPerAdvance,
        .timeScale = 1.0,
    };
    return true;
}

bool SosClockSetTimeScale(SosClock *clock, double timeScale)
{
    if (clock == NULL || !isfinite(timeScale) || timeScale < 0.0 ||
        timeScale > SOS_CLOCK_MAX_TIME_SCALE)
    {
        return false;
    }
    clock->timeScale = timeScale;
    if (timeScale == 0.0)
    {
        clock->pendingTicks = 0.0;
    }
    return true;
}

uint32_t SosClockAdvance(SosClock *clock, double realSeconds)
{
    if (clock == NULL || clock->ticksPerSecond == 0u || !isfinite(realSeconds) ||
        realSeconds <= 0.0)
    {
        return 0u;
    }

    const double owed =
        clock->pendingTicks + realSeconds * clock->timeScale * (double)clock->ticksPerSecond;
    const double whole = floor(owed);
    uint32_t ticks = clock->maxTicksPerAdvance;
    if (whole < (double)clock->maxTicksPerAdvance)
    {
        ticks = (uint32_t)whole;
        clock->pendingTicks = owed - whole;
    }
    else
    {
        /* Whatever exceeds the limit is gone, including the fraction: the next
         * frame starts fresh instead of inheriting a debt it cannot repay. */
        const double dropped = whole - (double)ticks;
        /* 2^64 is exact in a double; anything below it converts exactly. */
        const uint64_t lost = dropped >= 18446744073709551616.0 ? UINT64_MAX : (uint64_t)dropped;
        clock->droppedTicks =
            lost > UINT64_MAX - clock->droppedTicks ? UINT64_MAX : clock->droppedTicks + lost;
        clock->pendingTicks = 0.0;
    }
    clock->tick += ticks;
    return ticks;
}

double SosClockTickSeconds(const SosClock *clock)
{
    return clock == NULL || clock->ticksPerSecond == 0u ? 0.0 : 1.0 / (double)clock->ticksPerSecond;
}

float SosClockInterpolation(const SosClock *clock)
{
    if (clock == NULL || !(clock->pendingTicks >= 0.0) || clock->pendingTicks >= 1.0)
    {
        return 0.0f;
    }
    return (float)clock->pendingTicks;
}
