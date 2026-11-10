#include "sim/plants.h"

#include "sim/calendar.h"
#include "sim/cells.h"
#include "sim/random.h"

#include <stddef.h>
#include <stdlib.h>

/* Chances are out of 65536 cells. */
#define CHANCE_BITS 16u
#define CHANCE_MASK ((UINT64_C(1) << CHANCE_BITS) - 1u)
/* A plant keeps half a metre from its cell's edges. */
#define CELL_MARGIN (SOS_METRES(1) / 2)
#define CELL_SPAN (SOS_PLANT_CELL_UNITS - 2 * CELL_MARGIN)
#define NEVER_HARVESTED INT64_MIN
#define INITIAL_CAPACITY 64u

typedef struct Species
{
    uint32_t chance;
    SosMaterial ground;
    /* Time from a harvest until the plant bears food again. */
    int64_t regrowth;
} Species;

static const Species speciesTable[SOS_PLANT_SPECIES_COUNT] = {
    /* One 4 m cell in eight: a bush per 128 square metres of meadow. */
    [SOS_PLANT_BERRY_BUSH] = {.chance = 8192u,
                              .ground = SOS_MATERIAL_GRASS,
                              .regrowth = 20 * SOS_TICKS_PER_DAY},
};

/* A harvested plant. An empty slot has ripeTick NEVER_HARVESTED. */
typedef struct Harvest
{
    int64_t cellX;
    int64_t cellY;
    int64_t ripeTick;
} Harvest;

struct SosPlants
{
    uint64_t seed;
    /* Open addressing with linear probing; capacity is a power of two and at
     * most half of it is used. */
    Harvest *harvests;
    uint32_t capacity;
    uint32_t count;
};

SosPlants *SosPlantsCreate(uint64_t worldSeed)
{
    SosPlants *plants = calloc(1u, sizeof(*plants));
    if (plants != NULL)
    {
        plants->seed = SosHashCombine(worldSeed, UINT64_C(0x706c616e7473)); /* "plants" */
    }
    return plants;
}

void SosPlantsDestroy(SosPlants *plants)
{
    if (plants != NULL)
    {
        free(plants->harvests);
        free(plants);
    }
}

static uint32_t Slot(const SosPlants *plants, int64_t cellX, int64_t cellY)
{
    const uint64_t hash = SosHashCombine(SosHash64((uint64_t)cellX), (uint64_t)cellY);
    return (uint32_t)(hash & (plants->capacity - 1u));
}

static const Harvest *FindHarvest(const SosPlants *plants, int64_t cellX, int64_t cellY)
{
    if (plants->capacity == 0u)
    {
        return NULL;
    }
    for (uint32_t slot = Slot(plants, cellX, cellY);; slot = (slot + 1u) & (plants->capacity - 1u))
    {
        const Harvest *harvest = &plants->harvests[slot];
        if (harvest->ripeTick == NEVER_HARVESTED)
        {
            return NULL;
        }
        if (harvest->cellX == cellX && harvest->cellY == cellY)
        {
            return harvest;
        }
    }
}

static void Place(SosPlants *plants, Harvest harvest)
{
    uint32_t slot = Slot(plants, harvest.cellX, harvest.cellY);
    while (plants->harvests[slot].ripeTick != NEVER_HARVESTED)
    {
        slot = (slot + 1u) & (plants->capacity - 1u);
    }
    plants->harvests[slot] = harvest;
}

/* Rebuilds the table with `capacity` slots, keeping only plants still bare
 * at `tick`: the others look exactly like plants never harvested. */
static bool Rebuild(SosPlants *plants, uint32_t capacity, uint64_t tick)
{
    Harvest *harvests = malloc((size_t)capacity * sizeof(*harvests));
    if (harvests == NULL)
    {
        return false;
    }
    for (uint32_t slot = 0u; slot < capacity; ++slot)
    {
        harvests[slot].ripeTick = NEVER_HARVESTED;
    }
    Harvest *old = plants->harvests;
    const uint32_t oldCapacity = plants->capacity;
    plants->harvests = harvests;
    plants->capacity = capacity;
    plants->count = 0u;
    for (uint32_t slot = 0u; slot < oldCapacity; ++slot)
    {
        if (old[slot].ripeTick > (int64_t)tick)
        {
            Place(plants, old[slot]);
            plants->count += 1u;
        }
    }
    free(old);
    return true;
}

static bool Remember(SosPlants *plants, int64_t cellX, int64_t cellY, int64_t ripeTick,
                     uint64_t tick)
{
    Harvest *existing = (Harvest *)FindHarvest(plants, cellX, cellY);
    if (existing != NULL)
    {
        existing->ripeTick = ripeTick;
        return true;
    }
    if ((plants->count + 1u) * 2u > plants->capacity)
    {
        /* Forget the plants that bear food again before growing the table. */
        if (plants->capacity != 0u && !Rebuild(plants, plants->capacity, tick))
        {
            return false;
        }
        if ((plants->count + 1u) * 2u > plants->capacity)
        {
            if (plants->capacity > UINT32_MAX / 2u)
            {
                return false;
            }
            const uint32_t capacity =
                plants->capacity == 0u ? INITIAL_CAPACITY : plants->capacity * 2u;
            if (!Rebuild(plants, capacity, tick))
            {
                return false;
            }
        }
    }
    Place(plants, (Harvest){.cellX = cellX, .cellY = cellY, .ripeTick = ripeTick});
    plants->count += 1u;
    return true;
}

/* The plant the world function puts into a cell, before any harvest. */
static bool Generate(const SosPlants *plants, const SosTerrain *terrain, int64_t cellX,
                     int64_t cellY, SosPlant *outPlant)
{
    const uint64_t hash =
        SosHashCombine(SosHashCombine(plants->seed, (uint64_t)cellX), (uint64_t)cellY);
    const uint32_t roll = (uint32_t)(hash & CHANCE_MASK);
    uint32_t threshold = 0u;
    SosPlantSpecies species = SOS_PLANT_NONE;
    for (uint32_t candidate = 1u; candidate < SOS_PLANT_SPECIES_COUNT; ++candidate)
    {
        threshold += speciesTable[candidate].chance;
        if (roll < threshold)
        {
            species = (SosPlantSpecies)candidate;
            break;
        }
    }
    if (species == SOS_PLANT_NONE)
    {
        return false;
    }
    const int64_t x = cellX * SOS_PLANT_CELL_UNITS + CELL_MARGIN +
                      (int64_t)((hash >> CHANCE_BITS) % (uint64_t)CELL_SPAN);
    const int64_t y =
        cellY * SOS_PLANT_CELL_UNITS + CELL_MARGIN + (int64_t)((hash >> 40u) % (uint64_t)CELL_SPAN);
    const SosTerrainSurface surface = SosTerrainSurfaceAt(terrain, x, y);
    if (surface.material != speciesTable[species].ground)
    {
        return false;
    }
    *outPlant = (SosPlant){
        .cellX = cellX,
        .cellY = cellY,
        .position = {x, y, surface.z},
        .species = species,
        .ripe = true,
    };
    return true;
}

static bool PlantInCell(const SosPlants *plants, const SosTerrain *terrain, int64_t cellX,
                        int64_t cellY, uint64_t tick, SosPlant *outPlant)
{
    if (!Generate(plants, terrain, cellX, cellY, outPlant))
    {
        return false;
    }
    const Harvest *harvest = FindHarvest(plants, cellX, cellY);
    outPlant->ripe = harvest == NULL || harvest->ripeTick <= (int64_t)tick;
    return true;
}

bool SosPlantsInCell(const SosPlants *plants, const SosTerrain *terrain, int64_t cellX,
                     int64_t cellY, uint64_t tick, SosPlant *outPlant)
{
    if (plants == NULL || terrain == NULL || outPlant == NULL)
    {
        return false;
    }
    return PlantInCell(plants, terrain, cellX, cellY, tick, outPlant);
}

/* State of one nearest-ripe search, shared with its visitor. */
typedef struct RipeSearch
{
    const SosPlants *plants;
    const SosTerrain *terrain;
    uint64_t tick;
    SosPosition around;
    uint64_t radiusSquared;
    /* The squared distance of the best bush found, UINT64_MAX for none. */
    uint64_t best;
    SosPlant plant;
} RipeSearch;

static void VisitRipe(int64_t cellX, int64_t cellY, void *context)
{
    RipeSearch *search = context;
    SosPlant plant;
    if (!PlantInCell(search->plants, search->terrain, cellX, cellY, search->tick, &plant) ||
        !plant.ripe)
    {
        return;
    }
    const uint64_t distance = SosCellsGroundDistanceSquared(search->around, plant.position);
    if (distance <= search->radiusSquared && distance < search->best)
    {
        search->best = distance;
        search->plant = plant;
    }
}

bool SosPlantsFindRipe(const SosPlants *plants, const SosTerrain *terrain, SosPosition around,
                       int64_t radius, uint64_t tick, SosPlant *outPlant)
{
    if (plants == NULL || terrain == NULL || outPlant == NULL || radius < 0)
    {
        return false;
    }
    RipeSearch search = {.plants = plants,
                         .terrain = terrain,
                         .tick = tick,
                         .around = around,
                         .radiusSquared = SosCellsSquared((uint64_t)radius),
                         .best = UINT64_MAX};
    if (!SosCellsVisitRings(around, radius, SOS_PLANT_CELL_SHIFT, SOS_PLANT_QUERY_MAX_CELLS,
                            &search.best, VisitRipe, &search) ||
        search.best == UINT64_MAX)
    {
        return false;
    }
    *outPlant = search.plant;
    return true;
}

bool SosPlantsHarvest(SosPlants *plants, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                      uint64_t tick)
{
    SosPlant plant;
    if (plants == NULL || terrain == NULL ||
        !PlantInCell(plants, terrain, cellX, cellY, tick, &plant) || !plant.ripe)
    {
        return false;
    }
    (void)Remember(plants, cellX, cellY, (int64_t)tick + speciesTable[plant.species].regrowth,
                   tick);
    return true;
}

/* State of one box query, shared with its visitor. */
typedef struct PlantCollect
{
    const SosPlants *plants;
    const SosTerrain *terrain;
    uint64_t tick;
    SosPlant *outPlants;
    uint32_t capacity;
    uint32_t found;
} PlantCollect;

static void CollectPlant(int64_t cellX, int64_t cellY, void *context)
{
    PlantCollect *collect = context;
    SosPlant plant;
    if (PlantInCell(collect->plants, collect->terrain, cellX, cellY, collect->tick, &plant))
    {
        if (collect->found < collect->capacity)
        {
            collect->outPlants[collect->found] = plant;
        }
        collect->found += 1u;
    }
}

uint32_t SosPlantsQuery(const SosPlants *plants, const SosTerrain *terrain, int64_t minX,
                        int64_t minY, int64_t maxX, int64_t maxY, uint64_t tick,
                        SosPlant *outPlants, uint32_t capacity)
{
    if (plants == NULL || terrain == NULL || (outPlants == NULL && capacity != 0u))
    {
        return 0u;
    }
    PlantCollect collect = {.plants = plants,
                            .terrain = terrain,
                            .tick = tick,
                            .outPlants = outPlants,
                            .capacity = capacity,
                            .found = 0u};
    SosCellsVisitBox(minX, minY, maxX, maxY, SOS_PLANT_CELL_SHIFT, SOS_PLANT_QUERY_MAX_CELLS,
                     CollectPlant, &collect);
    return collect.found;
}

uint64_t SosPlantsHash(const SosPlants *plants, uint64_t tick)
{
    if (plants == NULL)
    {
        return 0u;
    }
    /* A sum does not depend on where the table keeps each plant. */
    uint64_t sum = 0u;
    for (uint32_t slot = 0u; slot < plants->capacity; ++slot)
    {
        const Harvest *harvest = &plants->harvests[slot];
        if (harvest->ripeTick > (int64_t)tick)
        {
            sum += SosHash64(SosHashCombine(
                SosHashCombine(SosHash64((uint64_t)harvest->cellX), (uint64_t)harvest->cellY),
                (uint64_t)harvest->ripeTick));
        }
    }
    return SosHashCombine(plants->seed, sum);
}
