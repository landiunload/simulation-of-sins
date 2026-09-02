#ifndef SIMULATION_OF_SINS_APP_APPLICATION_H
#define SIMULATION_OF_SINS_APP_APPLICATION_H

typedef enum SimulationRunMode
{
    SIMULATION_RUN_INTERACTIVE = 0,
    SIMULATION_RUN_RENDER_SMOKE,
    SIMULATION_RUN_REBASE_RENDER_SMOKE,
} SimulationRunMode;

int SimulationApplicationRun(SimulationRunMode mode);

#endif
