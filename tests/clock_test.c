#include "sim/clock.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "clock test failed: %s\n", message);
        exit(1);
    }
}

static SosClock NewClock(void)
{
    SosClock clock;
    Expect(SosClockInit(&clock, 30u, 0.25), "clock initializes");
    return clock;
}

static void TestInitialization(void)
{
    SosClock clock;
    Expect(!SosClockInit(NULL, 30u, 0.25), "a missing clock is rejected");
    Expect(!SosClockInit(&clock, 0u, 0.25), "zero rate is rejected");
    Expect(!SosClockInit(&clock, SOS_CLOCK_MAX_TICKS_PER_SECOND + 1u, 0.25),
           "a rate above the limit is rejected");
    Expect(!SosClockInit(&clock, 30u, 0.0) && !SosClockInit(&clock, 30u, NAN) &&
               !SosClockInit(&clock, 30u, INFINITY),
           "an advance limit must be positive and finite");
    clock = NewClock();
    Expect(clock.tick == 0u && clock.timeScale == 1.0 && clock.pendingTicks == 0.0 &&
               clock.droppedSeconds == 0.0,
           "a new clock starts at tick zero in real time");
    Expect(fabs(SosClockTickSeconds(&clock) - 1.0 / 30.0) < 1e-15, "tick length follows the rate");
}

static void TestRealTime(void)
{
    SosClock clock = NewClock();
    Expect(SosClockAdvance(&clock, 1.0 / 60.0) == 0u, "half a tick runs nothing");
    Expect(fabsf(SosClockInterpolation(&clock) - 0.5f) < 1e-6f, "half a tick is carried");
    Expect(fabs(SosClockSecondsToNextTick(&clock) - 1.0 / 60.0) < 1e-12,
           "the next tick is half a tick away");
    Expect(SosClockAdvance(&clock, 1.0 / 60.0) == 1u, "the carried half completes a tick");
    Expect(clock.tick == 1u, "the tick is counted");

    /* Frame slicing must not change the result: ten seconds as 600 frames give
     * exactly the rate, with nothing lost and nothing invented. */
    clock = NewClock();
    uint64_t total = 0u;
    for (int frame = 0; frame < 600; ++frame)
    {
        total += SosClockAdvance(&clock, 1.0 / 60.0);
    }
    Expect(total == 300u && clock.tick == 300u && clock.droppedSeconds == 0.0,
           "ten seconds at 60 frames per second give 300 ticks");
}

static void TestInvalidIntervals(void)
{
    SosClock clock = NewClock();
    Expect(SosClockAdvance(&clock, -1.0) == 0u, "negative time runs nothing");
    Expect(SosClockAdvance(&clock, 0.0) == 0u, "zero time runs nothing");
    Expect(SosClockAdvance(&clock, NAN) == 0u, "NaN runs nothing");
    Expect(SosClockAdvance(&clock, INFINITY) == 0u, "infinity runs nothing");
    Expect(clock.tick == 0u && clock.pendingTicks == 0.0, "invalid time leaves no trace");
    Expect(SosClockAdvance(NULL, 1.0) == 0u, "a missing clock runs nothing");
}

static void TestStallLimit(void)
{
    SosClock clock = NewClock();
    Expect(SosClockAdvance(&clock, 2.0) == 7u, "a stall counts at most the advance limit");
    Expect(fabs(clock.droppedSeconds - 1.75) < 1e-12 && fabs(clock.pendingTicks - 0.5) < 1e-12,
           "the rest of the stall is dropped and counted");
    Expect(SosClockAdvance(&clock, 1.0 / 60.0) == 1u, "the carried fraction survives a stall");
}

static void TestTimeScale(void)
{
    SosClock clock = NewClock();
    Expect(!SosClockSetTimeScale(&clock, -1.0) && !SosClockSetTimeScale(&clock, NAN) &&
               !SosClockSetTimeScale(&clock, SOS_CLOCK_MAX_TIME_SCALE * 2.0) &&
               !SosClockSetTimeScale(NULL, 1.0),
           "invalid time scales are rejected");
    Expect(clock.timeScale == 1.0, "a rejected scale keeps the previous one");

    Expect(SosClockSetTimeScale(&clock, 10.0), "fast-forward is accepted");
    Expect(SosClockAdvance(&clock, 0.1) == 30u, "ten times faster gives ten times the ticks");

    /* A game year per real second: the whole limited advance is available to
     * the world, which decides how coarsely to step through it. */
    Expect(SosClockSetTimeScale(&clock, SOS_CLOCK_MAX_TIME_SCALE), "a year per second is accepted");
    Expect(SosClockAdvance(&clock, 0.25) == 236682000u,
           "a quarter second at a year per second is a quarter of a game year");

    Expect(SosClockSetTimeScale(&clock, 0.5), "slow motion is accepted");
    clock.pendingTicks = 0.0;
    Expect(SosClockAdvance(&clock, 0.1) == 1u && fabs(clock.pendingTicks - 0.5) < 1e-12,
           "half speed gives half the ticks");
    Expect(SosClockSetTimeScale(&clock, 0.0), "pause is accepted");
    Expect(clock.pendingTicks == 0.0, "pausing discards the carried fraction");
    Expect(SosClockAdvance(&clock, 5.0) == 0u, "a paused world does not move");
    Expect(SosClockSecondsToNextTick(&clock) < 0.0, "a paused world has no next tick");
}

int main(void)
{
    TestInitialization();
    TestRealTime();
    TestInvalidIntervals();
    TestStallLimit();
    TestTimeScale();
    return 0;
}
