#include "game/foundation_world.h"
#include "game/frame_timing.h"
#include "game/rebase_policy.h"

#include "world/world.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int failures;

#define EXPECT(condition)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fputs(__FILE__ ": expectation failed: " #condition "\n", stderr);                      \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void TestFoundationWorld(void)
{
    World *world = WorldCreate(NULL);
    EXPECT(world != NULL);
    if (world == NULL)
    {
        return;
    }

    uint64_t initialRevision = WorldGetRevision(world);
    EXPECT(SimulationFoundationWorldPopulate(world));
    EXPECT(SimulationFoundationWorldValidate(world));
    uint64_t populatedRevision = WorldGetRevision(world);
    EXPECT(populatedRevision > initialRevision);
    EXPECT(!SimulationFoundationWorldPopulate(world));
    EXPECT(WorldGetRevision(world) == populatedRevision);
    WorldDestroy(world);
}

static void TestOriginShift(void)
{
    SimulationOriginShift shift;
    const double nearOrigin[3] = {511.9, -511.9, 2.0};
    EXPECT(SimulationOriginShiftPlan(nearOrigin, &shift));
    EXPECT(!shift.required);

    double distant[3] = {513.25, -513.75, 1025.5};
    EXPECT(SimulationOriginShiftPlan(distant, &shift));
    EXPECT(shift.required);
    EXPECT(shift.block[0] == 512);
    EXPECT(shift.block[1] == -512);
    EXPECT(shift.block[2] == 1024);
    SimulationOriginShiftApply(distant, &shift);
    EXPECT(fabs(distant[0] - 1.25) < 0.000001);
    EXPECT(fabs(distant[1] + 1.75) < 0.000001);
    EXPECT(fabs(distant[2] - 1.5) < 0.000001);

    const double invalid[3] = {NAN, 0.0, 0.0};
    EXPECT(!SimulationOriginShiftPlan(invalid, &shift));
    EXPECT(!shift.required);

    const double positiveLimit[3] = {ldexp(1.0, 63), 0.0, 0.0};
    EXPECT(!SimulationOriginShiftPlan(positiveLimit, &shift));
    const double negativeLimit[3] = {-ldexp(1.0, 63), 0.0, 0.0};
    EXPECT(SimulationOriginShiftPlan(negativeLimit, &shift));
    EXPECT(shift.block[0] == INT64_MIN);
    const double negativeOverflow[3] = {nextafter(-ldexp(1.0, 63), -INFINITY), 0.0, 0.0};
    EXPECT(!SimulationOriginShiftPlan(negativeOverflow, &shift));
    EXPECT(!shift.required);

    EXPECT(SimulationBlockToChunkFloor(64) == 1);
    EXPECT(SimulationBlockToChunkFloor(63) == 0);
    EXPECT(SimulationBlockToChunkFloor(0) == 0);
    EXPECT(SimulationBlockToChunkFloor(-1) == -1);
    EXPECT(SimulationBlockToChunkFloor(-64) == -1);
    EXPECT(SimulationBlockToChunkFloor(-65) == -2);
    EXPECT(SimulationBlockToChunkFloor(INT64_MIN) == INT64_MIN / 64);

    volatile int64_t runtimeNegativeMultiple = -INT64_C(7443687346329987200);
    EXPECT(SimulationBlockToChunkFloor(runtimeNegativeMultiple) == -INT64_C(116307614786406050));
}

static void TestFrameTiming(void)
{
    EXPECT(SimulationFrameDeltaSeconds(10.0, 10.016) > 0.015f);
    EXPECT(SimulationFrameDeltaSeconds(10.0, 10.016) < 0.017f);
    EXPECT(SimulationFrameDeltaSeconds(10.0, 9.0) == 0.0f);
    EXPECT(SimulationFrameDeltaSeconds(10.0, 12.0) == 0.1f);
    EXPECT(SimulationFrameDeltaSeconds(NAN, 12.0) == 0.0f);
}

int main(void)
{
    TestFoundationWorld();
    TestOriginShift();
    TestFrameTiming();
    return failures == 0 ? 0 : 1;
}
