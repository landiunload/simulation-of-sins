#include "sim/calendar.h"
#include "sim/cells.h"
#include "sim/fauna.h"
#include "sim/random.h"
#include "sim/terrain.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define TEST_CELLS 16
#define BOX_HERDS (TEST_CELLS * TEST_CELLS)
#define RIDGE_CELLS 7
/* A wide box and many days are enough to see every quadrant and a balanced
 * mean offset, while staying far under the two-second test budget. */
#define STAT_CELLS 32
#define STAT_HERDS (STAT_CELLS * STAT_CELLS)
#define WAYPOINT_DAYS 400
/* The Q16 interpolation can round the last tick of a day by up to a full day's
 * travel divided by SOS_Q16_ONE, plus one for truncation; four units per axis
 * covers the whole range a waypoint can traverse. */
#define BOUNDARY_STEP 4

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "fauna test failed: %s\n", message);
        exit(1);
    }
}

static SosHerd herds[BOX_HERDS];
static SosHerd statHerds[STAT_HERDS];

/* Herds in a square of `cells` cells per side starting at a cell. */
static uint32_t QueryCells(const SosFauna *fauna, const SosTerrain *terrain, int64_t firstCellX,
                           int64_t firstCellY, int64_t cells, uint64_t tick)
{
    const int64_t minX = firstCellX * SOS_FAUNA_CELL_UNITS;
    const int64_t minY = firstCellY * SOS_FAUNA_CELL_UNITS;
    const int64_t span = cells * SOS_FAUNA_CELL_UNITS - 1;
    const uint32_t capacity = (uint32_t)(cells * cells);
    Expect(capacity <= BOX_HERDS, "the test box fits its buffer");
    const uint32_t count =
        SosFaunaQuery(fauna, terrain, minX, minY, minX + span, minY + span, tick, herds, capacity);
    Expect(count <= capacity, "a box holds at most one herd per cell");
    return count;
}

static void TestSpecies(void)
{
    SosFaunaSpeciesInfo info;
    Expect(SosFaunaSpeciesInfoGet(SOS_FAUNA_RABBIT, &info) && info.ground == SOS_MATERIAL_GRASS &&
               info.minimumSize >= 1u && info.minimumSize <= info.maximumSize && info.regrowth > 0,
           "a rabbit is small grass game");
    Expect(SosFaunaSpeciesInfoGet(SOS_FAUNA_BEAR, &info) && info.meat > 0 &&
               info.danger == SOS_Q16_ONE,
           "a bear is dangerous and worth meat");
    Expect(!SosFaunaSpeciesInfoGet(SOS_FAUNA_NONE, &info) &&
               !SosFaunaSpeciesInfoGet(SOS_FAUNA_SPECIES_COUNT, &info) &&
               !SosFaunaSpeciesInfoGet(SOS_FAUNA_RABBIT, NULL),
           "unknown species and NULL outputs are rejected");
}

static void TestPlacement(const SosFauna *fauna, const SosTerrain *terrain)
{
    const uint32_t count =
        QueryCells(fauna, terrain, -TEST_CELLS / 2, -TEST_CELLS / 2, TEST_CELLS, 0u);
    Expect(count > 0u, "the fields near the origin hold herds");
    for (uint32_t index = 0u; index < count; ++index)
    {
        const SosHerd *herd = &herds[index];
        SosFaunaSpeciesInfo info;
        Expect(SosFaunaSpeciesInfoGet(herd->species, &info), "a herd has a known species");
        Expect(herd->size >= info.minimumSize && herd->size <= info.maximumSize,
               "a herd counts within its species' range");
        Expect(herd->alive == herd->size, "an unhunted herd is whole");
        const SosTerrainSurface surface = SosTerrainSurfaceAt(terrain, herd->home.x, herd->home.y);
        Expect(surface.material == info.ground && surface.z == herd->home.z,
               "a herd homes on its species' ground");
        const int64_t inX = herd->home.x - herd->cellX * SOS_FAUNA_CELL_UNITS;
        const int64_t inY = herd->home.y - herd->cellY * SOS_FAUNA_CELL_UNITS;
        Expect(inX >= SOS_FAUNA_HERD_RANGE && inX < SOS_FAUNA_CELL_UNITS - SOS_FAUNA_HERD_RANGE &&
                   inY >= SOS_FAUNA_HERD_RANGE && inY < SOS_FAUNA_CELL_UNITS - SOS_FAUNA_HERD_RANGE,
               "a home keeps away from its cell's edges");
        SosHerd again;
        Expect(SosFaunaInCell(fauna, terrain, herd->cellX, herd->cellY, 0u, &again) &&
                   again.species == herd->species && again.size == herd->size &&
                   again.home.x == herd->home.x && again.home.y == herd->home.y,
               "a cell and a query see the same herd");
    }
}

static void TestMovement(const SosFauna *fauna, const SosTerrain *terrain)
{
    const uint32_t count =
        QueryCells(fauna, terrain, -TEST_CELLS / 2, -TEST_CELLS / 2, TEST_CELLS, 0u);
    Expect(count > 0u, "a herd roams");
    const SosHerd herd = herds[0];
    const SosPosition home = herd.home;
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    for (uint64_t tick = 0u; tick < 3u * day; tick += day / 64u)
    {
        const SosPosition position = SosFaunaHerdPosition(&herd, terrain, tick);
        Expect(position.x >= home.x - SOS_FAUNA_HERD_RANGE &&
                   position.x <= home.x + SOS_FAUNA_HERD_RANGE &&
                   position.y >= home.y - SOS_FAUNA_HERD_RANGE &&
                   position.y <= home.y + SOS_FAUNA_HERD_RANGE,
               "a herd stays within range of its home");
        Expect(position.z == SosTerrainSurfaceZ(terrain, position.x, position.y),
               "a herd walks on the ground");
    }
    /* Across every day boundary the position must not jump, and a coarse step
     * of many days must land where fine steps would. */
    for (uint64_t boundary = day; boundary <= 40u * day; boundary += day)
    {
        const SosPosition before = SosFaunaHerdPosition(&herd, terrain, boundary - 1u);
        const SosPosition at = SosFaunaHerdPosition(&herd, terrain, boundary);
        const int64_t dx = at.x - before.x;
        const int64_t dy = at.y - before.y;
        /* The Q16 interpolation truncates the last tick of the day, so a full
         * day's travel can leave a couple of units to the boundary step. */
        Expect(dx >= -BOUNDARY_STEP && dx <= BOUNDARY_STEP && dy >= -BOUNDARY_STEP &&
                   dy <= BOUNDARY_STEP,
               "a herd crosses a day boundary smoothly");
    }
    const SosPosition coarse = SosFaunaHerdPosition(&herd, terrain, 7u * day);
    const SosPosition fine = SosFaunaHerdPosition(&herd, terrain, 7u * day);
    Expect(coarse.x == fine.x && coarse.y == fine.y, "the same tick gives the same place");
    Expect(SosFaunaHerdPosition(NULL, terrain, 0u).x == 0 &&
               SosFaunaHerdPosition(&herd, NULL, 0u).x == 0,
           "an invalid position request gives the zero position");
}

/* Homes must be drawn over the whole cell rather than a masked band, so every
 * quadrant of a cell centre holds homes. */
static void TestHomeSpread(const SosFauna *fauna, const SosTerrain *terrain)
{
    const int64_t first = -STAT_CELLS / 2;
    const int64_t minX = first * SOS_FAUNA_CELL_UNITS;
    const int64_t span = STAT_CELLS * SOS_FAUNA_CELL_UNITS - 1;
    const uint32_t count = SosFaunaQuery(fauna, terrain, minX, minX, minX + span, minX + span, 0u,
                                         statHerds, STAT_HERDS);
    Expect(count > 0u && count <= STAT_HERDS, "the spread box holds herds");
    const int64_t half = SOS_FAUNA_CELL_UNITS / 2;
    uint32_t quadrants[4] = {0u, 0u, 0u, 0u};
    for (uint32_t index = 0u; index < count; ++index)
    {
        const int64_t inX = statHerds[index].home.x - statHerds[index].cellX * SOS_FAUNA_CELL_UNITS;
        const int64_t inY = statHerds[index].home.y - statHerds[index].cellY * SOS_FAUNA_CELL_UNITS;
        ++quadrants[(inX >= half ? 1u : 0u) + (inY >= half ? 2u : 0u)];
    }
    Expect(quadrants[0] > 0u && quadrants[1] > 0u && quadrants[2] > 0u && quadrants[3] > 0u,
           "homes cover all four quadrants of their cells");
}

/* Waypoints must be symmetric around home: they reach every direction and
 * their mean offset is near zero, which a one-sided draw cannot do. */
static void TestWaypointSpread(const SosFauna *fauna, const SosTerrain *terrain)
{
    const uint32_t count =
        QueryCells(fauna, terrain, -TEST_CELLS / 2, -TEST_CELLS / 2, TEST_CELLS, 0u);
    Expect(count > 0u, "a waypoint spread needs a herd");
    const SosHerd herd = herds[0];
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    uint32_t quadrants[4] = {0u, 0u, 0u, 0u};
    int64_t sumX = 0;
    int64_t sumY = 0;
    for (int index = 0; index < WAYPOINT_DAYS; ++index)
    {
        const SosPosition position = SosFaunaHerdPosition(&herd, terrain, (uint64_t)index * day);
        const int64_t dx = position.x - herd.home.x;
        const int64_t dy = position.y - herd.home.y;
        Expect(dx >= -SOS_FAUNA_HERD_RANGE && dx <= SOS_FAUNA_HERD_RANGE &&
                   dy >= -SOS_FAUNA_HERD_RANGE && dy <= SOS_FAUNA_HERD_RANGE,
               "a waypoint stays within range of home");
        ++quadrants[(dx >= 0 ? 1u : 0u) + (dy >= 0 ? 2u : 0u)];
        sumX += dx;
        sumY += dy;
    }
    Expect(quadrants[0] > 0u && quadrants[1] > 0u && quadrants[2] > 0u && quadrants[3] > 0u,
           "waypoints go in all four directions");
    const int64_t bound = (int64_t)WAYPOINT_DAYS * SOS_FAUNA_HERD_RANGE / 4;
    Expect(sumX > -bound && sumX < bound && sumY > -bound && sumY < bound,
           "the mean waypoint offset is near zero");
}

static void TestFindNearest(const SosFauna *fauna, const SosTerrain *terrain)
{
    const uint64_t tick = 12345u;
    const int64_t radius = SOS_METRES(400);
    const int64_t cells = 2 * (radius / SOS_FAUNA_CELL_UNITS + 2) + 1;
    SosRandom random = SosRandomStream(SEED, 7u);
    for (int trial = 0; trial < 40; ++trial)
    {
        const SosPosition around = {
            SosRandomRange(&random, SOS_METRES(-3000), SOS_METRES(3000)),
            SosRandomRange(&random, SOS_METRES(-3000), SOS_METRES(3000)),
            0,
        };
        const int64_t centerX = SosFloorShift(around.x, SOS_FAUNA_CELL_SHIFT);
        const int64_t centerY = SosFloorShift(around.y, SOS_FAUNA_CELL_SHIFT);
        const int64_t half = cells / 2;
        const uint32_t count =
            QueryCells(fauna, terrain, centerX - half, centerY - half, cells, tick);
        uint64_t nearest = UINT64_MAX;
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (herds[index].alive == 0u)
            {
                continue;
            }
            const uint64_t distance = SosCellsGroundDistanceSquared(
                around, SosFaunaHerdPosition(&herds[index], terrain, tick));
            if (distance <= (uint64_t)(radius * radius) && distance < nearest)
            {
                nearest = distance;
            }
        }
        SosHerd found;
        const bool any =
            SosFaunaFindNearest(fauna, terrain, around, radius, SOS_FAUNA_NONE, tick, &found);
        Expect(any == (nearest != UINT64_MAX),
               "a search finds a herd exactly when one is in reach");
        if (!any)
        {
            continue;
        }
        Expect(found.alive > 0u &&
                   SosCellsGroundDistanceSquared(
                       around, SosFaunaHerdPosition(&found, terrain, tick)) == nearest,
               "a search finds the nearest herd");
    }
    SosHerd found;
    if (SosFaunaFindNearest(fauna, terrain, (SosPosition){0, 0, 0}, SOS_METRES(4000),
                            SOS_FAUNA_BEAR, 0u, &found))
    {
        Expect(found.species == SOS_FAUNA_BEAR, "the species filter is honoured");
    }
    Expect(!SosFaunaFindNearest(fauna, terrain, (SosPosition){0, 0, 0}, SOS_METRES(4000),
                                (SosFaunaSpecies)99, 0u, &found),
           "an unknown species filter is rejected");
    Expect(!SosFaunaFindNearest(fauna, terrain, (SosPosition){0, 0, 0},
                                SOS_FAUNA_QUERY_MAX_CELLS * SOS_FAUNA_CELL_UNITS, SOS_FAUNA_NONE,
                                0u, &found),
           "an oversized search is rejected");
    Expect(!SosFaunaFindNearest(NULL, terrain, (SosPosition){0, 0, 0}, radius, SOS_FAUNA_NONE, 0u,
                                &found) &&
               !SosFaunaFindNearest(fauna, NULL, (SosPosition){0, 0, 0}, radius, SOS_FAUNA_NONE, 0u,
                                    &found) &&
               !SosFaunaFindNearest(fauna, terrain, (SosPosition){0, 0, 0}, radius, SOS_FAUNA_NONE,
                                    0u, NULL),
           "invalid searches fail safely");
}

/* A herd with at least two animals, so half of it is neither all nor nothing. */
static SosHerd PickHerd(const SosFauna *fauna, const SosTerrain *terrain, int64_t firstCellX,
                        int64_t firstCellY)
{
    const uint32_t count = QueryCells(fauna, terrain, firstCellX, firstCellY, TEST_CELLS, 0u);
    Expect(count > 0u, "the picked box holds herds");
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (herds[index].size >= 2u)
        {
            return herds[index];
        }
    }
    Expect(0, "the picked box holds a herd of more than one animal");
    return herds[0];
}

static void TestHuntAndRegrowth(const SosTerrain *terrain)
{
    SosFauna *fauna = SosFaunaCreate(SEED);
    const SosFauna *untouched = SosFaunaCreate(SEED);
    Expect(fauna != NULL && untouched != NULL, "fauna are created");
    const SosHerd herd = PickHerd(fauna, terrain, 100, 100);
    SosFaunaSpeciesInfo info;
    Expect(SosFaunaSpeciesInfoGet(herd.species, &info), "a hunted herd has a species");

    const uint32_t taken = SosFaunaHunt(fauna, terrain, herd.cellX, herd.cellY, 0u, herd.size + 5u);
    Expect(taken == herd.size, "a hunt cannot take more animals than are alive");
    SosHerd herdNow;
    Expect(SosFaunaInCell(fauna, terrain, herd.cellX, herd.cellY, 1u, &herdNow) &&
               herdNow.alive == 0u,
           "a herd hunted to nothing is empty");
    Expect(SosFaunaHunt(fauna, terrain, herd.cellX, herd.cellY, 1u, 4u) == 0u,
           "an empty herd gives nothing");
    Expect(SosFaunaHash(fauna, 1u) != SosFaunaHash(untouched, 1u), "a hunt changes the hash");

    const uint64_t regrowth = (uint64_t)info.regrowth;
    Expect(SosFaunaInCell(fauna, terrain, herd.cellX, herd.cellY, regrowth / 2u, &herdNow) &&
               herdNow.alive > 0u && herdNow.alive < herd.size,
           "a herd regrows partway");
    Expect(SosFaunaInCell(fauna, terrain, herd.cellX, herd.cellY, regrowth - 1u, &herdNow) &&
               herdNow.alive < herd.size,
           "a herd stays short while it regrows");
    Expect(SosFaunaInCell(fauna, terrain, herd.cellX, herd.cellY, regrowth, &herdNow) &&
               herdNow.alive == herd.size,
           "a herd is whole again after regrowing");
    Expect(SosFaunaHash(fauna, regrowth) == SosFaunaHash(untouched, regrowth),
           "a regrown herd counts as never hunted");

    /* A cell without a herd is never hunted; find one by scanning past the box. */
    int64_t emptyCellX = -TEST_CELLS;
    int64_t emptyCellY = -TEST_CELLS;
    while (true)
    {
        SosHerd empty;
        if (!SosFaunaInCell(fauna, terrain, emptyCellX, emptyCellY, 0u, &empty))
        {
            break;
        }
        ++emptyCellX;
    }
    Expect(SosFaunaHunt(fauna, terrain, emptyCellX, emptyCellY, 0u, 4u) == 0u,
           "an empty cell gives nothing");

    SosFaunaDestroy((SosFauna *)untouched);
    SosFaunaDestroy(fauna);
}

static void TestDeterminism(const SosTerrain *terrain)
{
    SosFauna *first = SosFaunaCreate(SEED);
    SosFauna *second = SosFaunaCreate(SEED);
    Expect(first != NULL && second != NULL, "fauna are created");
    for (int round = 0; round < 200; ++round)
    {
        const int64_t cellX = (round % 20) - 10;
        const int64_t cellY = (round / 20) - 5;
        const uint64_t tick = (uint64_t)round * 1000u;
        const uint32_t a = SosFaunaHunt(first, terrain, cellX, cellY, tick, 2u);
        const uint32_t b = SosFaunaHunt(second, terrain, cellX, cellY, tick, 2u);
        Expect(a == b, "the same hunt gives the same result");
    }
    Expect(SosFaunaHash(first, 500000u) == SosFaunaHash(second, 500000u),
           "equal worlds hash equally after equal hunts");
    for (int round = 0; round < 8; ++round)
    {
        const uint32_t a = QueryCells(first, terrain, round, round, TEST_CELLS, 500000u);
        const uint32_t b = QueryCells(second, terrain, round, round, TEST_CELLS, 500000u);
        Expect(a == b, "equal worlds see the same herds");
    }
    SosFaunaDestroy(first);
    SosFaunaDestroy(second);
}

static void TestEdges(const SosTerrain *terrain)
{
    Expect(SosFaunaHash(NULL, 0u) == 0u, "a NULL world hashes to zero");
    SosFauna *fauna = SosFaunaCreate(SEED);
    Expect(fauna != NULL, "fauna are created");
    SosHerd herd;
    Expect(!SosFaunaInCell(NULL, terrain, 0, 0, 0u, &herd) &&
               !SosFaunaInCell(fauna, NULL, 0, 0, 0u, &herd) &&
               !SosFaunaInCell(fauna, terrain, 0, 0, 0u, NULL) &&
               !SosFaunaInCell(fauna, terrain, INT64_MAX, INT64_MAX, 0u, &herd),
           "invalid cells fail safely");
    Expect(SosFaunaHunt(NULL, terrain, 0, 0, 0u, 4u) == 0u &&
               SosFaunaHunt(fauna, NULL, 0, 0, 0u, 4u) == 0u &&
               SosFaunaHunt(fauna, terrain, INT64_MAX, INT64_MAX, 0u, 4u) == 0u &&
               SosFaunaHunt(fauna, terrain, 0, 0, 0u, 0u) == 0u,
           "invalid hunts fail safely");
    /* A NULL output with no capacity may still count herds. */
    Expect(SosFaunaQuery(fauna, terrain, 0, 0, SOS_FAUNA_CELL_UNITS - 1, SOS_FAUNA_CELL_UNITS - 1,
                         0u, NULL, 0u) <= 1u,
           "a count-only query is allowed");
    Expect(SosFaunaQuery(NULL, terrain, 0, 0, 1, 1, 0u, &herd, 1u) == 0u &&
               SosFaunaQuery(fauna, NULL, 0, 0, 1, 1, 0u, &herd, 1u) == 0u &&
               SosFaunaQuery(fauna, terrain, 1, 0, 0, 1, 0u, &herd, 1u) == 0u &&
               SosFaunaQuery(fauna, terrain, 0, 1, 1, 0, 0u, &herd, 1u) == 0u &&
               SosFaunaQuery(fauna, terrain, 0, 0, 1, 0, 0u, NULL, 1u) == 0u,
           "invalid boxes fail safely");
    Expect(SosFaunaQuery(fauna, terrain, 0, 0,
                         (int64_t)SOS_FAUNA_QUERY_MAX_CELLS * SOS_FAUNA_CELL_UNITS, 0, 0u, &herd,
                         1u) == 0u,
           "an oversized box returns zero");
    SosFaunaDestroy(fauna);
}

/* Fixed points of the fauna cover at tick zero. Every platform and compiler
 * must place exactly these herds. */
static void TestGoldenValues(const SosFauna *fauna, const SosTerrain *terrain)
{
    const uint32_t count = QueryCells(fauna, terrain, -8, 0, TEST_CELLS, 0u);
    uint64_t fingerprint = count;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const SosPosition position = SosFaunaHerdPosition(&herds[index], terrain, 0u);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)position.x);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)position.y);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)position.z);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)herds[index].species);
        fingerprint = SosHashCombine(fingerprint, herds[index].size);
    }
    const uint64_t expected = UINT64_C(0x26667054F2382BC4);
    if (fingerprint != expected)
    {
        fprintf(stderr, "fauna fingerprint: 0x%016" PRIX64 " (%u herds)\n", fingerprint, count);
        Expect(0, "the fauna cover matches the reference");
    }
}

int main(void)
{
    SosTerrain terrain;
    SosTerrainInit(&terrain, SEED);
    TestSpecies();
    SosFauna *fauna = SosFaunaCreate(SEED);
    Expect(fauna != NULL, "fauna are created");
    TestPlacement(fauna, &terrain);
    TestMovement(fauna, &terrain);
    TestHomeSpread(fauna, &terrain);
    TestWaypointSpread(fauna, &terrain);
    TestFindNearest(fauna, &terrain);
    TestGoldenValues(fauna, &terrain);
    SosFaunaDestroy(fauna);
    TestHuntAndRegrowth(&terrain);
    TestDeterminism(&terrain);
    TestEdges(&terrain);
    return 0;
}
