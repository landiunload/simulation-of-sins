#include "sim/plants.h"

#include "sim/calendar.h"
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

static uint64_t GroundDistanceSquared(SosPosition from, SosPosition to)
{
    const int64_t dx = to.x - from.x;
    const int64_t dy = to.y - from.y;
    return (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
}

bool SosPlantsFindRipe(const SosPlants *plants, const SosTerrain *terrain, SosPosition around,
                       int64_t radius, uint64_t tick, SosPlant *outPlant)
{
    const int64_t maxRadius = (SOS_PLANT_QUERY_MAX_CELLS / 2 - 1) * SOS_PLANT_CELL_UNITS;
    if (plants == NULL || terrain == NULL || outPlant == NULL || radius < 0 || radius > maxRadius)
    {
        return false;
    }
    const int64_t centerX = SosFloorShift(around.x, SOS_PLANT_CELL_SHIFT);
    const int64_t centerY = SosFloorShift(around.y, SOS_PLANT_CELL_SHIFT);
    const uint64_t radiusSquared = (uint64_t)(radius * radius);
    const int64_t rings = radius / SOS_PLANT_CELL_UNITS + 1;
    uint64_t best = UINT64_MAX;
    /* Rings of cells around the centre, nearest first. Every point of ring r
     * lies at least (r - 1) cells from `around`, so the search stops once a
     * ring cannot hold anything nearer than the best plant found. */
    for (int64_t ring = 0; ring <= rings; ++ring)
    {
        const int64_t gap = (ring - 1) * SOS_PLANT_CELL_UNITS;
        if (ring > 1 && (uint64_t)(gap * gap) > (best < radiusSquared ? best : radiusSquared))
        {
            break;
        }
        for (int64_t dy = -ring; dy <= ring; ++dy)
        {
            const bool edgeRow = dy == -ring || dy == ring;
            for (int64_t dx = -ring; dx <= ring; dx += edgeRow ? 1 : 2 * ring)
            {
                SosPlant plant;
                if (!PlantInCell(plants, terrain, centerX + dx, centerY + dy, tick, &plant) ||
                    !plant.ripe)
                {
                    continue;
                }
                const uint64_t distance = GroundDistanceSquared(around, plant.position);
                if (distance <= radiusSquared && distance < best)
                {
                    best = distance;
                    *outPlant = plant;
                }
            }
        }
    }
    return best != UINT64_MAX;
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

uint32_t SosPlantsQuery(const SosPlants *plants, const SosTerrain *terrain, int64_t minX,
                        int64_t minY, int64_t maxX, int64_t maxY, uint64_t tick,
                        SosPlant *outPlants, uint32_t capacity)
{
    if (plants == NULL || terrain == NULL || (outPlants == NULL && capacity != 0u) || maxX < minX ||
        maxY < minY)
    {
        return 0u;
    }
    const int64_t firstX = SosFloorShift(minX, SOS_PLANT_CELL_SHIFT);
    const int64_t firstY = SosFloorShift(minY, SOS_PLANT_CELL_SHIFT);
    const int64_t lastX = SosFloorShift(maxX, SOS_PLANT_CELL_SHIFT);
    const int64_t lastY = SosFloorShift(maxY, SOS_PLANT_CELL_SHIFT);
    if (lastX - firstX >= SOS_PLANT_QUERY_MAX_CELLS || lastY - firstY >= SOS_PLANT_QUERY_MAX_CELLS)
    {
        return 0u;
    }
    uint32_t found = 0u;
    for (int64_t cellY = firstY; cellY <= lastY; ++cellY)
    {
        for (int64_t cellX = firstX; cellX <= lastX; ++cellX)
        {
            SosPlant plant;
            if (PlantInCell(plants, terrain, cellX, cellY, tick, &plant))
            {
                if (found < capacity)
                {
                    outPlants[found] = plant;
                }
                ++found;
            }
        }
    }
    return found;
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
