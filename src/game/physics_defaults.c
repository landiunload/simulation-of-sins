#include "game/physics_defaults.h"

#include <string.h>

#define SIMULATION_PHYSICS_MAX_THREADS 64u

bool SimulationParsePhysicsThreads(const char *text, uint32_t *outThreads)
{
    if (text == NULL || outThreads == NULL || text[0] == '\0')
    {
        return false;
    }
    uint32_t value = 0u;
    for (const char *digit = text; *digit != '\0'; ++digit)
    {
        if (*digit < '0' || *digit > '9')
        {
            return false;
        }
        value = value * 10u + (uint32_t)(*digit - '0');
        if (value > SIMULATION_PHYSICS_MAX_THREADS)
        {
            return false;
        }
    }
    if (value == 0u)
    {
        return false;
    }
    *outThreads = value;
    return true;
}

uint32_t SimulationDefaultPhysicsThreads(uint32_t logicalProcessorCount)
{
    uint32_t threads = logicalProcessorCount;
    if (threads > SIMULATION_PHYSICS_DEFAULT_THREAD_CAP)
    {
        threads = SIMULATION_PHYSICS_DEFAULT_THREAD_CAP;
    }
    return threads == 0u ? 1u : threads;
}

uint32_t SimulationPhysicsThreads(const char *text, uint32_t logicalProcessorCount)
{
    uint32_t parsed = 0u;
    if (SimulationParsePhysicsThreads(text, &parsed))
    {
        return parsed;
    }
    return SimulationDefaultPhysicsThreads(logicalProcessorCount);
}

bool SimulationUseSpatialIndex(const char *text)
{
    return text != NULL && strcmp(text, "tree") == 0;
}
