#include "sim/clock.h"

#include <math.h>
#include <stddef.h>

bool SosClockInit(SosClock *clock, uint32_t ticksPerSecond, double maxAdvanceSeconds)
{
    if (clock == NULL || ticksPerSecond == 0u || ticksPerSecond > SOS_CLOCK_MAX_TICKS_PER_SECOND ||
        !isfinite(maxAdvanceSeconds) || !(maxAdvanceSeconds > 0.0))
    {
        return false;
    }
    *clock = (SosClock){
        .ticksPerSecond = ticksPerSecond,
        .maxAdvanceSeconds = maxAdvanceSeconds,
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

uint64_t SosClockAdvance(SosClock *clock, double realSeconds)
{
    if (clock == NULL || clock->ticksPerSecond == 0u || !isfinite(realSeconds) ||
        realSeconds <= 0.0)
    {
        return 0u;
    }
    if (realSeconds > clock->maxAdvanceSeconds)
    {
        clock->droppedSeconds += realSeconds - clock->maxAdvanceSeconds;
        realSeconds = clock->maxAdvanceSeconds;
    }
    /* At most maxAdvanceSeconds * 3.2e7 * 1000 ticks: far inside both the
     * exact integer range of a double and uint64_t. */
    const double owed =
        clock->pendingTicks + realSeconds * clock->timeScale * (double)clock->ticksPerSecond;
    const double whole = floor(owed);
    clock->pendingTicks = owed - whole;
    const uint64_t ticks = (uint64_t)whole;
    clock->tick += ticks;
    return ticks;
}

double SosClockSecondsToNextTick(const SosClock *clock)
{
    if (clock == NULL || !(clock->timeScale > 0.0) || clock->ticksPerSecond == 0u)
    {
        return -1.0;
    }
    return (1.0 - clock->pendingTicks) / (clock->timeScale * (double)clock->ticksPerSecond);
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
