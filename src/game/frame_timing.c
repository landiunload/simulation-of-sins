#include "game/frame_timing.h"

#include <math.h>

#define SIMULATION_MAX_FRAME_DELTA_SECONDS 0.1

float SimulationFrameDeltaSeconds(double previousSeconds, double currentSeconds)
{
    if (!isfinite(previousSeconds) || !isfinite(currentSeconds) ||
        currentSeconds <= previousSeconds)
    {
        return 0.0f;
    }

    double delta = currentSeconds - previousSeconds;
    if (delta > SIMULATION_MAX_FRAME_DELTA_SECONDS)
    {
        delta = SIMULATION_MAX_FRAME_DELTA_SECONDS;
    }
    return (float)delta;
}
