#ifndef SOS_SIM_WORLD_H
#define SOS_SIM_WORLD_H

#include "sim/beings.h"
#include "sim/terrain.h"

#include <stdint.h>

/* The simulated world: terrain, the beings living on it and time. Everything
 * in it follows from the seed and the sequence of advances, so the same seed
 * and the same advances replay the same history on every machine. */

/* An advance runs at most this many steps. Small advances step tick by tick;
 * a fast-forwarded one splits evenly, so a game year per second still costs a
 * bounded amount of work per frame. */
#define SOS_WORLD_MAX_STEPS_PER_ADVANCE 256u

typedef struct SosWorld SosWorld;

typedef struct SosWorldDesc
{
    uint64_t seed;
} SosWorldDesc;

/* A new world at tick zero with its founders: a man and a woman of twenty. */
SosWorld *SosWorldCreate(const SosWorldDesc *desc);
void SosWorldDestroy(SosWorld *world);

void SosWorldAdvance(SosWorld *world, uint64_t ticks);

uint64_t SosWorldTick(const SosWorld *world);
uint64_t SosWorldSeed(const SosWorld *world);
const SosTerrain *SosWorldTerrain(const SosWorld *world);
const SosBeings *SosWorldBeings(const SosWorld *world);
/* Folds the whole state into one value for replay and cross-machine checks. */
uint64_t SosWorldHash(const SosWorld *world);

#endif
