#include "sim/calendar.h"
#include "sim/plants.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define REGROWTH (20 * SOS_TICKS_PER_DAY)
#define BOX_CELLS 64

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "plants test failed: %s\n", message);
        exit(1);
    }
}

static SosPlant box[BOX_CELLS * BOX_CELLS];

/* Plants in a square of BOX_CELLS cells per side starting at a cell. */
static uint32_t QueryBox(const SosPlants *plants, const SosTerrain *terrain, int64_t cellX,
                         int64_t cellY, uint64_t tick)
{
    const int64_t minX = cellX * SOS_PLANT_CELL_UNITS;
    const int64_t minY = cellY * SOS_PLANT_CELL_UNITS;
    const int64_t span = BOX_CELLS * SOS_PLANT_CELL_UNITS - 1;
    const uint32_t count = SosPlantsQuery(plants, terrain, minX, minY, minX + span, minY + span,
                                          tick, box, BOX_CELLS * BOX_CELLS);
    Expect(count <= BOX_CELLS * BOX_CELLS, "a box holds at most one plant per cell");
    return count;
}

static void TestPlacement(const SosPlants *plants, const SosTerrain *terrain)
{
    const uint32_t count = QueryBox(plants, terrain, -BOX_CELLS / 2, -BOX_CELLS / 2, 0u);
    /* Near the origin the seed gives meadows: about one cell in eight. */
    const uint32_t cells = BOX_CELLS * BOX_CELLS;
    Expect(count > cells / 12u && count < cells / 6u, "bushes cover a meadow at their density");
    for (uint32_t index = 0u; index < count; ++index)
    {
        const SosPlant *plant = &box[index];
        Expect(plant->species == SOS_PLANT_BERRY_BUSH && plant->ripe,
               "a bush never harvested bears berries");
        const int64_t inX = plant->position.x - plant->cellX * SOS_PLANT_CELL_UNITS;
        const int64_t inY = plant->position.y - plant->cellY * SOS_PLANT_CELL_UNITS;
        Expect(inX >= SOS_METRES(1) / 2 && inX < SOS_PLANT_CELL_UNITS - SOS_METRES(1) / 2 &&
                   inY >= SOS_METRES(1) / 2 && inY < SOS_PLANT_CELL_UNITS - SOS_METRES(1) / 2,
               "a bush keeps away from its cell's edges");
        const SosTerrainSurface surface =
            SosTerrainSurfaceAt(terrain, plant->position.x, plant->position.y);
        Expect(surface.material == SOS_MATERIAL_GRASS && surface.z == plant->position.z,
               "a bush grows on the grass surface");
        SosPlant single;
        Expect(SosPlantsInCell(plants, terrain, plant->cellX, plant->cellY, 0u, &single) &&
                   single.position.x == plant->position.x && single.position.y == plant->position.y,
               "a cell and a query see the same bush");
    }
}

static void FindEmptyCell(const SosPlants *plants, const SosTerrain *terrain, int64_t *outX,
                          int64_t *outY)
{
    for (int64_t cellX = 0;; ++cellX)
    {
        SosPlant plant;
        if (!SosPlantsInCell(plants, terrain, cellX, 3, 0u, &plant))
        {
            *outX = cellX;
            *outY = 3;
            return;
        }
    }
}

static void TestHarvest(SosPlants *plants, const SosTerrain *terrain)
{
    const uint32_t count = QueryBox(plants, terrain, 100, 100, 0u);
    Expect(count > 0u, "a meadow has bushes");
    const SosPlant bush = box[0];
    const uint64_t tick = 1000u;
    Expect(SosPlantsHarvest(plants, terrain, bush.cellX, bush.cellY, tick),
           "a ripe bush is harvested");
    Expect(!SosPlantsHarvest(plants, terrain, bush.cellX, bush.cellY, tick + 1u),
           "a bare bush gives nothing");
    SosPlant later;
    Expect(SosPlantsInCell(plants, terrain, bush.cellX, bush.cellY, tick + REGROWTH - 1u, &later) &&
               !later.ripe,
           "a bush stays bare while it regrows");
    Expect(SosPlantsInCell(plants, terrain, bush.cellX, bush.cellY, tick + REGROWTH, &later) &&
               later.ripe,
           "a bush bears berries again after regrowing");
    Expect(SosPlantsHarvest(plants, terrain, bush.cellX, bush.cellY, tick + REGROWTH),
           "a regrown bush is harvested again");

    int64_t emptyX = 0;
    int64_t emptyY = 0;
    FindEmptyCell(plants, terrain, &emptyX, &emptyY);
    Expect(!SosPlantsHarvest(plants, terrain, emptyX, emptyY, tick), "an empty cell gives nothing");
}

static uint64_t GroundDistanceSquared(SosPosition from, SosPosition to)
{
    const int64_t dx = to.x - from.x;
    const int64_t dy = to.y - from.y;
    return (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
}

static void TestFindRipe(SosPlants *plants, const SosTerrain *terrain)
{
    const uint64_t tick = 5000u;
    const int64_t radius = SOS_METRES(64);
    SosRandom random = SosRandomStream(SEED, 7u);
    for (int trial = 0; trial < 40; ++trial)
    {
        const SosPosition around = {
            SosRandomRange(&random, SOS_METRES(-2000), SOS_METRES(2000)),
            SosRandomRange(&random, SOS_METRES(-2000), SOS_METRES(2000)),
            0,
        };
        /* Every ripe bush within the radius, by brute force. */
        const int64_t first = SosFloorShift(around.x - radius, SOS_PLANT_CELL_SHIFT);
        const int64_t firstY = SosFloorShift(around.y - radius, SOS_PLANT_CELL_SHIFT);
        const uint32_t count = QueryBox(plants, terrain, first, firstY, tick);
        uint64_t nearest = UINT64_MAX;
        for (uint32_t index = 0u; index < count; ++index)
        {
            const uint64_t distance = GroundDistanceSquared(around, box[index].position);
            if (box[index].ripe && distance <= (uint64_t)(radius * radius) && distance < nearest)
            {
                nearest = distance;
            }
        }
        SosPlant found;
        const bool any = SosPlantsFindRipe(plants, terrain, around, radius, tick, &found);
        Expect(any == (nearest != UINT64_MAX),
               "a search finds a bush exactly when one is in reach");
        if (!any)
        {
            continue;
        }
        Expect(found.ripe && GroundDistanceSquared(around, found.position) == nearest,
               "a search finds the nearest ripe bush");
        /* Harvest it: the next search finds another one no nearer. */
        Expect(SosPlantsHarvest(plants, terrain, found.cellX, found.cellY, tick),
               "the found bush is harvested");
        SosPlant next;
        if (SosPlantsFindRipe(plants, terrain, around, radius, tick, &next))
        {
            Expect(next.cellX != found.cellX || next.cellY != found.cellY,
                   "a harvested bush is not found again");
            Expect(GroundDistanceSquared(around, next.position) >= nearest,
                   "the next bush is no nearer");
        }
    }
    SosPlant unused;
    Expect(!SosPlantsFindRipe(plants, terrain, (SosPosition){0, 0, 0},
                              SOS_PLANT_QUERY_MAX_CELLS * SOS_PLANT_CELL_UNITS, tick, &unused),
           "an oversized search is rejected");
}

/* Thousands of harvests grow the layer; bushes that bear berries again are
 * forgotten and the hash treats them as never harvested. */
static void TestMemory(const SosTerrain *terrain)
{
    SosPlants *plants = SosPlantsCreate(SEED);
    SosPlants *untouched = SosPlantsCreate(SEED);
    Expect(plants != NULL && untouched != NULL, "plants are created");
    Expect(SosPlantsHash(plants, 0u) == SosPlantsHash(untouched, 0u), "equal plants hash equally");
    uint32_t harvested = 0u;
    for (int64_t row = 0; row < 4; ++row)
    {
        const uint32_t count = QueryBox(plants, terrain, -200, row * BOX_CELLS, 0u);
        for (uint32_t index = 0u; index < count; ++index)
        {
            harvested += SosPlantsHarvest(plants, terrain, box[index].cellX, box[index].cellY, 0u);
        }
    }
    Expect(harvested > 1000u, "thousands of bushes are harvested");
    Expect(SosPlantsHash(plants, 0u) != SosPlantsHash(untouched, 0u), "harvests change the hash");
    const uint32_t bare = QueryBox(plants, terrain, -200, 0, 1u);
    for (uint32_t index = 0u; index < bare; ++index)
    {
        Expect(!box[index].ripe, "every harvested bush is bare");
    }

    /* Twenty days later every one of them bears berries again; harvesting
     * elsewhere then forgets them while the layer grows. */
    const uint64_t later = (uint64_t)REGROWTH;
    Expect(SosPlantsHash(plants, later) == SosPlantsHash(untouched, later),
           "regrown bushes count as never harvested");
    const uint32_t count = QueryBox(plants, terrain, 300, 0, later);
    for (uint32_t index = 0u; index < count; ++index)
    {
        (void)SosPlantsHarvest(plants, terrain, box[index].cellX, box[index].cellY, later);
    }
    const uint32_t regrown = QueryBox(plants, terrain, -200, 0, later);
    for (uint32_t index = 0u; index < regrown; ++index)
    {
        Expect(box[index].ripe, "regrown bushes bear berries");
    }
    SosPlantsDestroy(untouched);
    SosPlantsDestroy(plants);
}

/* Fixed points of the plant cover. Every platform and compiler must place
 * exactly these bushes. */
static void TestGoldenValues(const SosPlants *plants, const SosTerrain *terrain)
{
    const uint32_t count = QueryBox(plants, terrain, -1000, 2500, 0u);
    uint64_t fingerprint = count;
    for (uint32_t index = 0u; index < count; ++index)
    {
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.x);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.y);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.z);
    }
    const uint64_t expected = UINT64_C(0xF37222ED263F3331);
    if (fingerprint != expected)
    {
        fprintf(stderr, "plant fingerprint: 0x%016" PRIX64 " (%u bushes)\n", fingerprint, count);
        Expect(0, "the plant cover matches the reference");
    }
}

int main(void)
{
    SosTerrain terrain;
    SosTerrainInit(&terrain, SEED);
    SosPlants *plants = SosPlantsCreate(SEED);
    Expect(plants != NULL, "plants are created");
    TestPlacement(plants, &terrain);
    TestHarvest(plants, &terrain);
    TestFindRipe(plants, &terrain);
    TestMemory(&terrain);
    TestGoldenValues(plants, &terrain);
    SosPlantsDestroy(plants);
    return 0;
}
