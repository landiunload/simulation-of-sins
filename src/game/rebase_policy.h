#ifndef SIMULATION_OF_SINS_GAME_REBASE_POLICY_H
#define SIMULATION_OF_SINS_GAME_REBASE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define SIMULATION_REBASE_THRESHOLD_CHUNKS 8

typedef struct SimulationOriginShift
{
    int64_t block[3];
    bool required;
} SimulationOriginShift;

int64_t SimulationBlockToChunkFloor(int64_t block);
bool SimulationOriginShiftPlan(const double localPosition[3], SimulationOriginShift *outShift);
void SimulationOriginShiftApply(double localPosition[3], const SimulationOriginShift *shift);

#endif
