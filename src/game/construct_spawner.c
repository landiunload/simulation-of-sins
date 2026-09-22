#include "game/construct_spawner.h"

#include "game/foundation_world.h"

#include <stddef.h>

static double NextSpin(uint64_t *state)
{
    *state = *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return ((double)(*state >> 33) / 2147483648.0 - 0.5) * 0.7;
}

static bool SpawnConstruct(void *context, const double position[3])
{
    ConstructSystem *system = context;
    SimulationCubeField *field = system->field;
    if (field->spawnCounter >= SIMULATION_CONSTRUCT_SPAWN_LIMIT)
        return false;

    ConstructBlock blocks[SIMULATION_CONSTRUCT_SPAWN_BLOCKS] = {0};
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        blocks[index].local[2] = (int32_t)index;
        blocks[index].material = SIMULATION_MATERIAL_ACCENT;
    }
    for (uint32_t index = 5u; index < SIMULATION_CONSTRUCT_SPAWN_BLOCKS; ++index)
    {
        blocks[index].local[0] = (int32_t)index - 4;
        blocks[index].local[2] = 4;
        blocks[index].material = SIMULATION_MATERIAL_MARKER;
    }

    uint32_t ordinal = (uint32_t)field->spawnCounter;
    uint32_t layer = ordinal / 100u;
    const double origin[3] = {
        position[0] + ((double)(ordinal % 10u) - 4.5) * 8.0 - 2.5,
        position[1] + ((double)((ordinal / 10u) % 10u) - 4.5) * 8.0 - 0.5,
        position[2] + (double)layer * 6.0,
    };
    uint64_t nextRandomState = field->randomState;
    double spin[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        spin[axis] = NextSpin(&nextRandomState);
    uint32_t slot = field->count;
    if (!ConstructSpawn(system, blocks, SIMULATION_CONSTRUCT_SPAWN_BLOCKS, origin, NULL))
        return false;
    // An allocation failure here is terminal for the tick, never a silent skip.
    if (!VoxelRigidBodyAddAngularVelocity(&field->bodies[slot], spin))
        return false;
    field->randomState = nextRandomState;
    return true;
}

bool SimulationConstructSpawnerAttach(ConstructSystem *system)
{
    if (system == NULL || system->world == NULL || system->field == NULL ||
        system->field->scratch == NULL || system->field->failed || system->field->count != 0u ||
        system->field->spawnCounter != 0u || ConstructActiveBodyCount(system) != 0u)
        return false;
    system->field->spawnBody = SpawnConstruct;
    system->field->spawnContext = system;
    system->field->spawnLimit = SIMULATION_CONSTRUCT_SPAWN_LIMIT;
    system->field->spawningStopped = false;
    return true;
}
