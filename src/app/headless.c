#include "app/headless.h"

#include "app/log.h"
#include "mod/application.h"
#include "sim/clock.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>

/* Never sleep longer than this, so a termination request is noticed quickly
 * even while the world is paused. */
#define SOS_HEADLESS_MAX_SLEEP_MILLISECONDS 50u

static uint32_t MillisecondsToNextTick(const SosClock *clock)
{
    const double ticksPerSecond = clock->timeScale * (double)clock->ticksPerSecond;
    if (!(ticksPerSecond > 0.0))
    {
        return SOS_HEADLESS_MAX_SLEEP_MILLISECONDS;
    }
    const double milliseconds = (1.0 - clock->pendingTicks) * 1000.0 / ticksPerSecond;
    if (!(milliseconds >= 1.0))
    {
        return 1u;
    }
    return milliseconds >= (double)SOS_HEADLESS_MAX_SLEEP_MILLISECONDS
               ? SOS_HEADLESS_MAX_SLEEP_MILLISECONDS
               : (uint32_t)ceil(milliseconds);
}

int SosHeadlessRun(const SosHeadlessOptions *options)
{
    SosClock clock;
    if (options == NULL ||
        !SosClockInit(&clock, SOS_CLOCK_DEFAULT_TICKS_PER_SECOND,
                      SOS_CLOCK_DEFAULT_MAX_TICKS_PER_ADVANCE) ||
        !SosClockSetTimeScale(&clock, options->timeScale))
    {
        SosLog(SOS_LOG_ERROR, "headless options are invalid");
        return 2;
    }
    if (LaiueApplicationInstallTerminationHandler() == 0u)
    {
        SosLog(SOS_LOG_WARNING, "termination handler unavailable; stop the process to exit");
    }
    SosLog(SOS_LOG_INFO, "headless world: %" PRIu32 " ticks per second, time scale %g",
           clock.ticksPerSecond, clock.timeScale);

    const double started = LaiueApplicationMonotonicSeconds();
    double previous = started;
    while (LaiueApplicationTerminationRequested() == 0u &&
           (options->ticks == 0u || clock.tick < options->ticks))
    {
        const double now = LaiueApplicationMonotonicSeconds();
        const uint32_t ticks = SosClockAdvance(&clock, now - previous);
        previous = now;
        /* A full catch-up batch means the machine is behind the requested
         * pace: keep going without sleeping. */
        if (ticks < clock.maxTicksPerAdvance)
        {
            LaiueApplicationSleepMilliseconds(MillisecondsToNextTick(&clock));
        }
    }
    LaiueApplicationRemoveTerminationHandler();
    SosLog(SOS_LOG_INFO, "stopped at tick %" PRIu64 " after %.3f s", clock.tick,
           LaiueApplicationMonotonicSeconds() - started);
    return 0;
}
