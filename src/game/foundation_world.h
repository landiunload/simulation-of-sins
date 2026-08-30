#ifndef SIMULATION_OF_SINS_GAME_FOUNDATION_WORLD_H
#define SIMULATION_OF_SINS_GAME_FOUNDATION_WORLD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct World World;

typedef enum SimulationMaterial
{
    SIMULATION_MATERIAL_FOUNDATION = 1,
    SIMULATION_MATERIAL_MARKER = 2,
    SIMULATION_MATERIAL_ACCENT = 3,
} SimulationMaterial;

bool SimulationFoundationWorldPopulate(World *world);
bool SimulationFoundationWorldValidate(World *world);

#endif
