#include "game/rebase_policy.h"

#include "world/world.h"

#include <math.h>
#include <stdint.h>

#define SIMULATION_SIGNED_COORDINATE_LIMIT 9223372036854775808.0

int64_t SimulationBlockToChunkFloor(int64_t block)
{
    int64_t chunk = block / CHUNK_SIZE;
    return block % CHUNK_SIZE < 0 ? chunk - 1 : chunk;
}

static bool AxisShift(double coordinate, int64_t *outShift)
{
    const double threshold = (double)(SIMULATION_REBASE_THRESHOLD_CHUNKS * CHUNK_SIZE);
    *outShift = 0;
    if (!isfinite(coordinate))
    {
        return false;
    }
    if (coordinate <= threshold && coordinate >= -threshold)
    {
        return true;
    }

    double chunks = coordinate / (double)CHUNK_SIZE;
    const double chunkCountLimit = SIMULATION_SIGNED_COORDINATE_LIMIT / (double)CHUNK_SIZE;
    if (chunks >= chunkCountLimit || chunks < -chunkCountLimit)
    {
        return false;
    }
    *outShift = (int64_t)chunks * CHUNK_SIZE;
    return true;
}

bool SimulationOriginShiftPlan(const double localPosition[3], SimulationOriginShift *outShift)
{
    if (localPosition == NULL || outShift == NULL)
    {
        return false;
    }

    outShift->required = false;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!AxisShift(localPosition[axis], &outShift->block[axis]))
        {
            outShift->block[0] = 0;
            outShift->block[1] = 0;
            outShift->block[2] = 0;
            outShift->required = false;
            return false;
        }
        outShift->required = outShift->required || outShift->block[axis] != 0;
    }
    return true;
}

void SimulationOriginShiftApply(double localPosition[3], const SimulationOriginShift *shift)
{
    if (localPosition == NULL || shift == NULL || !shift->required)
    {
        return;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        localPosition[axis] -= (double)shift->block[axis];
    }
}
