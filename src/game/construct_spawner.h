#ifndef SIMULATION_OF_SINS_GAME_CONSTRUCT_SPAWNER_H
#define SIMULATION_OF_SINS_GAME_CONSTRUCT_SPAWNER_H

#include "game/construct.h"

#define SIMULATION_CONSTRUCT_SPAWN_LIMIT 1000u
#define SIMULATION_CONSTRUCT_SPAWN_BLOCKS 9u

// Configure a fresh, empty field: 100 Cyrillic G bodies/second for 10 seconds
// of fixed simulation time, then stop spawning while physics keeps running.
// Each shape has a five-block upright and a five-block top sharing one block.
// The 10x10 drop grid has pitch 8; each 100-body layer starts 6 blocks higher.
// system is borrowed, immovable and must stay alive while the callback is used.
// ConstructSystemRelease detaches it; resetting the field requires reattaching.
bool SimulationConstructSpawnerAttach(ConstructSystem *system);

#endif
