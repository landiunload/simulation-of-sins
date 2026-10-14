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

static void TestInitialization(void)
{
    SosClock clock;
    Expect(!SosClockInit(NULL, 30u, 8u), "a missing clock is rejected");
    Expect(!SosClockInit(&clock, 0u, 8u), "zero rate is rejected");
    Expect(!SosClockInit(&clock, SOS_CLOCK_MAX_TICKS_PER_SECOND + 1u, 8u),
           "a rate above the limit is rejected");
    Expect(!SosClockInit(&clock, 30u, 0u), "a zero catch-up limit is rejected");
    Expect(SosClockInit(&clock, 30u, 8u), "a valid clock initializes");
    Expect(clock.tick == 0u && clock.timeScale == 1.0 && clock.pendingTicks == 0.0 &&
               clock.droppedTicks == 0u,
           "a new clock starts at tick zero in real time");
    Expect(fabs(SosClockTickSeconds(&clock) - 1.0 / 30.0) < 1e-15, "tick length follows the rate");
}

static void TestRealTime(void)
{
    SosClock clock;
    Expect(SosClockInit(&clock, 30u, 8u), "clock initializes");
    Expect(SosClockAdvance(&clock, 1.0 / 60.0) == 0u, "half a tick runs nothing");
    Expect(fabs(SosClockInterpolation(&clock) - 0.5f) < 1e-6f, "half a tick is carried");
    Expect(SosClockAdvance(&clock, 1.0 / 60.0) == 1u, "the carried half completes a tick");
    Expect(clock.tick == 1u, "the tick is counted");

    /* Frame slicing must not change the result: one second as 60 frames gives
     * exactly the rate, with nothing lost and nothing invented. */
    Expect(SosClockInit(&clock, 30u, 8u), "clock reinitializes");
    uint64_t total = 0u;
    for (int frame = 0; frame < 600; ++frame)
    {
        total += SosClockAdvance(&clock, 1.0 / 60.0);
    }
    Expect(total == 300u && clock.tick == 300u && clock.droppedTicks == 0u,
           "ten seconds at 60 frames per second give 300 ticks");
}

static void TestInvalidIntervals(void)
{
    SosClock clock;
    Expect(SosClockInit(&clock, 30u, 8u), "clock initializes");
    Expect(SosClockAdvance(&clock, -1.0) == 0u, "negative time runs nothing");
    Expect(SosClockAdvance(&clock, 0.0) == 0u, "zero time runs nothing");
    Expect(SosClockAdvance(&clock, NAN) == 0u, "NaN runs nothing");
    Expect(SosClockAdvance(&clock, INFINITY) == 0u, "infinity runs nothing");
    Expect(clock.tick == 0u && clock.pendingTicks == 0.0, "invalid time leaves no trace");
    Expect(SosClockAdvance(NULL, 1.0) == 0u, "a missing clock runs nothing");
}

static void TestCatchUpLimit(void)
{
    SosClock clock;
    Expect(SosClockInit(&clock, 30u, 8u), "clock initializes");
    Expect(SosClockAdvance(&clock, 2.0) == 8u, "a long stall runs at most the limit");
    Expect(clock.droppedTicks == 52u && clock.pendingTicks == 0.0,
           "the rest of the stall is dropped and counted");
    Expect(SosClockAdvance(&clock, 1.0 / 30.0) == 1u, "the next frame starts fresh");
    Expect(SosClockAdvance(&clock, 1e300) == 8u && clock.droppedTicks == UINT64_MAX,
           "an absurd interval saturates the dropped counter");
}

static void TestTimeScale(void)
{
    SosClock clock;
    Expect(SosClockInit(&clock, 30u, 1000u), "clock initializes");
    Expect(!SosClockSetTimeScale(&clock, -1.0) && !SosClockSetTimeScale(&clock, NAN) &&
               !SosClockSetTimeScale(&clock, SOS_CLOCK_MAX_TIME_SCALE * 2.0) &&
               !SosClockSetTimeScale(NULL, 1.0),
           "invalid time scales are rejected");
    Expect(clock.timeScale == 1.0, "a rejected scale keeps the previous one");

    Expect(SosClockSetTimeScale(&clock, 10.0), "fast-forward is accepted");
    Expect(SosClockAdvance(&clock, 1.0) == 300u, "ten times faster gives ten times the ticks");

    Expect(SosClockAdvance(&clock, 1.0 / 600.0) == 0u, "a sliver of time is carried");
    Expect(SosClockSetTimeScale(&clock, 0.0), "pause is accepted");
    Expect(clock.pendingTicks == 0.0, "pausing discards the carried fraction");
    Expect(SosClockAdvance(&clock, 5.0) == 0u, "a paused world does not move");
    Expect(clock.droppedTicks == 0u, "pause is not counted as dropped time");
}

int main(void)
{
    TestInitialization();
    TestRealTime();
    TestInvalidIntervals();
    TestCatchUpLimit();
    TestTimeScale();
    return 0;
}
