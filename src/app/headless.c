#include "app/headless.h"

#include "app/log.h"
#include "mod/application.h"
#include "sim/calendar.h"
#include "sim/clock.h"
#include "sim/world.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>

/* Never sleep longer than this, so a termination request is noticed quickly
 * even while the world is paused. */
#define SOS_HEADLESS_MAX_SLEEP_MILLISECONDS 50u

/* Zero when the next tick is due within a millisecond: a sleep that short
 * oversleeps, which would hold a fast-forwarded world far below its pace. */
static uint32_t MillisecondsToNextTick(const SosClock *clock)
{
    const double seconds = SosClockSecondsToNextTick(clock);
    if (seconds < 0.0)
    {
        return SOS_HEADLESS_MAX_SLEEP_MILLISECONDS;
    }
    const double milliseconds = seconds * 1000.0;
    if (!(milliseconds >= 1.0))
    {
        return 0u;
    }
    return milliseconds >= (double)SOS_HEADLESS_MAX_SLEEP_MILLISECONDS
               ? SOS_HEADLESS_MAX_SLEEP_MILLISECONDS
               : (uint32_t)ceil(milliseconds);
}

static void LogPopulation(const SosWorld *world, SosBeingsStats *reported)
{
    const SosBeingsStats stats = SosBeingsGetStats(SosWorldBeings(world));
    if (stats.births == reported->births && stats.deaths == reported->deaths)
    {
        return;
    }
    const SosDate date = SosCalendarDate(SosWorldTick(world));
    SosLog(SOS_LOG_INFO, "year %" PRId64 " day %d: %u living, %" PRIu64 " born, %" PRIu64 " died",
           date.year, date.day, SosBeingsLivingCount(SosWorldBeings(world)), stats.births,
           stats.deaths);
    *reported = stats;
}

int SosHeadlessRun(const SosHeadlessOptions *options)
{
    SosClock clock;
    if (options == NULL ||
        !SosClockInit(&clock, SOS_CLOCK_TICKS_PER_SECOND, SOS_CLOCK_DEFAULT_MAX_ADVANCE_SECONDS) ||
        !SosClockSetTimeScale(&clock, options->timeScale))
    {
        SosLog(SOS_LOG_ERROR, "headless options are invalid");
        return 2;
    }
    const SosWorldDesc desc = {.seed = options->seed};
    SosWorld *world = SosWorldCreate(&desc);
    if (world == NULL)
    {
        SosLog(SOS_LOG_ERROR, "the world could not be created");
        return 3;
    }
    if (LaiueApplicationInstallTerminationHandler() == 0u)
    {
        SosLog(SOS_LOG_WARNING, "termination handler unavailable; stop the process to exit");
    }
    SosLog(SOS_LOG_INFO, "headless world %" PRIu64 ": %" PRIu32 " ticks per second, time scale %g",
           options->seed, clock.ticksPerSecond, clock.timeScale);

    SosBeingsStats reported = {0};
    const double started = LaiueApplicationMonotonicSeconds();
    double previous = started;
    while (LaiueApplicationTerminationRequested() == 0u &&
           (options->ticks == 0u || SosWorldTick(world) < options->ticks))
    {
        const double now = LaiueApplicationMonotonicSeconds();
        uint64_t ticks = SosClockAdvance(&clock, now - previous);
        previous = now;
        if (options->ticks != 0u && ticks > options->ticks - SosWorldTick(world))
        {
            ticks = options->ticks - SosWorldTick(world);
        }
        SosWorldAdvance(world, ticks);
        LogPopulation(world, &reported);
        const uint32_t sleep = MillisecondsToNextTick(&clock);
        if (sleep != 0u)
        {
            LaiueApplicationSleepMilliseconds(sleep);
        }
    }
    LaiueApplicationRemoveTerminationHandler();
    const SosDate date = SosCalendarDate(SosWorldTick(world));
    SosLog(SOS_LOG_INFO,
           "stopped at tick %" PRIu64 " (year %" PRId64 " day %d %02d:%02d) after %.3f s, "
           "%u living, state %016" PRIx64,
           SosWorldTick(world), date.year, date.day, date.hour, date.minute,
           LaiueApplicationMonotonicSeconds() - started,
           SosBeingsLivingCount(SosWorldBeings(world)), SosWorldHash(world));
    SosWorldDestroy(world);
    return 0;
}
