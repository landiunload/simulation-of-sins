#include "sim/world.h"

#include "sim/calendar.h"
#include "sim/random.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define FOUNDER_AGE (20 * SOS_TICKS_PER_YEAR)

struct SosWorld
{
    uint64_t seed;
    uint64_t tick;
    SosTerrain terrain;
    SosPlants *plants;
    SosBeings *beings;
};

SosWorld *SosWorldCreate(const SosWorldDesc *desc)
{
    if (desc == NULL)
    {
        return NULL;
    }
    SosWorld *world = calloc(1u, sizeof(*world));
    if (world == NULL)
    {
        return NULL;
    }
    world->seed = desc->seed;
    SosTerrainInit(&world->terrain, desc->seed);
    world->plants = SosPlantsCreate(desc->seed);
    world->beings = SosBeingsCreate(desc->seed);
    const SosBeingSpawn founders[] = {
        {
            .sex = SOS_SEX_MALE,
            .birthTick = -FOUNDER_AGE,
            .position = {0, 0, 0},
            .knowledge = SOS_KNOWLEDGE_SPEECH | SOS_KNOWLEDGE_FORAGING,
        },
        {
            .sex = SOS_SEX_FEMALE,
            .birthTick = -FOUNDER_AGE,
            .position = {SOS_METRES(3) / 2, 0, 0},
            .knowledge = SOS_KNOWLEDGE_SPEECH | SOS_KNOWLEDGE_FORAGING,
        },
    };
    bool spawned = world->plants != NULL && world->beings != NULL;
    for (size_t index = 0u; spawned && index < sizeof(founders) / sizeof(founders[0]); ++index)
    {
        spawned = SosBeingsSpawn(world->beings, &world->terrain, 0u, &founders[index]) != 0u;
    }
    if (!spawned)
    {
        SosWorldDestroy(world);
        return NULL;
    }
    return world;
}

void SosWorldDestroy(SosWorld *world)
{
    if (world != NULL)
    {
        SosBeingsDestroy(world->beings);
        SosPlantsDestroy(world->plants);
        free(world);
    }
}

void SosWorldAdvance(SosWorld *world, uint64_t ticks)
{
    if (world == NULL || ticks == 0u)
    {
        return;
    }
    const uint64_t steps =
        ticks < SOS_WORLD_MAX_STEPS_PER_ADVANCE ? ticks : SOS_WORLD_MAX_STEPS_PER_ADVANCE;
    const SosEnvironment environment = {.terrain = &world->terrain, .plants = world->plants};
    const uint64_t length = ticks / steps;
    const uint64_t longer = ticks % steps;
    for (uint64_t step = 0u; step < steps; ++step)
    {
        const uint64_t stepTicks = length + (step < longer ? 1u : 0u);
        SosBeingsStep(world->beings, &environment, world->tick, stepTicks);
        world->tick += stepTicks;
    }
}

uint64_t SosWorldTick(const SosWorld *world)
{
    return world == NULL ? 0u : world->tick;
}

uint64_t SosWorldSeed(const SosWorld *world)
{
    return world == NULL ? 0u : world->seed;
}

const SosTerrain *SosWorldTerrain(const SosWorld *world)
{
    return world == NULL ? NULL : &world->terrain;
}

const SosPlants *SosWorldPlants(const SosWorld *world)
{
    return world == NULL ? NULL : world->plants;
}

const SosBeings *SosWorldBeings(const SosWorld *world)
{
    return world == NULL ? NULL : world->beings;
}

uint64_t SosWorldHash(const SosWorld *world)
{
    if (world == NULL)
    {
        return 0u;
    }
    const uint64_t hash = SosHashCombine(SosHashCombine(world->seed, world->tick),
                                         SosPlantsHash(world->plants, world->tick));
    return SosHashCombine(hash, SosBeingsHash(world->beings));
}
