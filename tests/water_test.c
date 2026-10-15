#include "sim/cells.h"
#include "sim/random.h"
#include "sim/terrain.h"
#include "sim/water.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define SIDE 64
/* Cells in a box that holds springs, ponds and lakes near the origin. */
#define MIXED_CELL_X 96
#define MIXED_CELL_Y (-32)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "water test failed: %s\n", message);
        exit(1);
    }
}

static SosWaterSource box[SIDE * SIDE];

/* Sources in a square of `side` cells per side starting at a cell. */
static uint32_t QueryBox(const SosWater *water, const SosTerrain *terrain, int64_t cellX,
                         int64_t cellY, int64_t side)
{
    const int64_t minX = cellX * SOS_WATER_CELL_UNITS;
    const int64_t minY = cellY * SOS_WATER_CELL_UNITS;
    const int64_t span = side * SOS_WATER_CELL_UNITS - 1;
    const uint32_t count =
        SosWaterQuery(water, terrain, minX, minY, minX + span, minY + span, box, SIDE * SIDE);
    Expect(count <= SIDE * SIDE, "a box holds at most one source per cell");
    return count;
}

/* The middle of a cell, where every source stands. */
static int64_t CellCentre(int64_t cell)
{
    return cell * SOS_WATER_CELL_UNITS + SOS_WATER_CELL_UNITS / 2;
}

static void TestDeterminism(const SosTerrain *terrain)
{
    SosWater water;
    SosWater again;
    SosWater other;
    SosWaterInit(&water, SEED);
    SosWaterInit(&again, SEED);
    SosWaterInit(&other, SEED + 1u);
    int differing = 0;
    for (int64_t cellX = -20; cellX <= 20; ++cellX)
    {
        for (int64_t cellY = -20; cellY <= 20; ++cellY)
        {
            SosWaterSource first;
            SosWaterSource second;
            SosWaterSource changed;
            const bool any = SosWaterInCell(&water, terrain, cellX, cellY, &first);
            Expect(any == SosWaterInCell(&again, terrain, cellX, cellY, &second),
                   "the same seed gives the same water");
            if (any)
            {
                Expect(first.cellX == second.cellX && first.cellY == second.cellY &&
                           first.position.x == second.position.x &&
                           first.position.y == second.position.y &&
                           first.position.z == second.position.z && first.kind == second.kind &&
                           first.quality == second.quality,
                       "a source replays exactly");
            }
            if (any != SosWaterInCell(&other, terrain, cellX, cellY, &changed))
            {
                ++differing;
            }
        }
    }
    Expect(differing > 50, "another seed moves the water");
}

/* Every source stands on the ground it names, never on snow, and its cell,
 * place, kind and quality agree with the rules. */
static void TestOnSurface(const SosWater *water, const SosTerrain *terrain)
{
    const uint32_t count = QueryBox(water, terrain, MIXED_CELL_X, MIXED_CELL_Y, SIDE);
    Expect(count > 0u, "the sampled box holds water");
    for (uint32_t index = 0u; index < count; ++index)
    {
        const SosWaterSource *source = &box[index];
        Expect(source->cellX >= MIXED_CELL_X && source->cellX < MIXED_CELL_X + SIDE &&
                   source->cellY >= MIXED_CELL_Y && source->cellY < MIXED_CELL_Y + SIDE,
               "a queried source belongs to a queried cell");
        Expect(source->position.x == CellCentre(source->cellX) &&
                   source->position.y == CellCentre(source->cellY),
               "a source stands at its cell's centre");
        const SosTerrainSurface surface =
            SosTerrainSurfaceAt(terrain, source->position.x, source->position.y);
        Expect(surface.z == source->position.z && surface.material != SOS_MATERIAL_SNOW,
               "a source lies on the surface, never on snow");
        Expect(source->kind == SOS_WATER_SPRING || source->kind == SOS_WATER_POND ||
                   source->kind == SOS_WATER_LAKE,
               "a source has a known kind");
        Expect(source->quality >= SOS_Q16_ONE / 4 && source->quality < SOS_Q16_ONE,
               "quality stays in its range");
        if (source->kind == SOS_WATER_LAKE)
        {
            Expect(surface.z < SOS_WATER_LEVEL, "a lake lies below the water level");
        }
        else
        {
            Expect(surface.z >= SOS_WATER_LEVEL, "a spring or pond stands on dry ground");
        }
        SosWaterSource single;
        Expect(SosWaterInCell(water, terrain, source->cellX, source->cellY, &single) &&
                   single.position.x == source->position.x && single.kind == source->kind,
               "a cell and a query see the same source");
    }
}

static void TestDensity(const SosWater *water, const SosTerrain *terrain)
{
    const uint32_t count = QueryBox(water, terrain, -SIDE / 2, -SIDE / 2, SIDE);
    const uint32_t cells = SIDE * SIDE;
    /* Near the origin the seed gives a few sources per hundred cells. */
    Expect(count > cells / 100u && count < cells / 6u, "water keeps a sensible density");
    const uint32_t mixed = QueryBox(water, terrain, MIXED_CELL_X, MIXED_CELL_Y, SIDE);
    uint32_t springs = 0u;
    uint32_t ponds = 0u;
    uint32_t lakes = 0u;
    for (uint32_t index = 0u; index < mixed; ++index)
    {
        springs += box[index].kind == SOS_WATER_SPRING;
        ponds += box[index].kind == SOS_WATER_POND;
        lakes += box[index].kind == SOS_WATER_LAKE;
    }
    Expect(springs > 0u && ponds > 0u && lakes > 0u, "springs, ponds and lakes all occur");
    Expect(springs < ponds, "springs are rarer than ponds");
}

static void TestFindNearest(const SosWater *water, const SosTerrain *terrain)
{
    const int64_t radius = SOS_METRES(128);
    const int64_t side = 2 * radius / SOS_WATER_CELL_UNITS + 3;
    SosRandom random = SosRandomStream(SEED, 11u);
    for (int trial = 0; trial < 40; ++trial)
    {
        const SosPosition around = {
            SosRandomRange(&random, SOS_METRES(-2000), SOS_METRES(2000)),
            SosRandomRange(&random, SOS_METRES(-2000), SOS_METRES(2000)),
            0,
        };
        /* Every source within the radius, by brute force. */
        const int64_t firstX = SosFloorShift(around.x - radius, SOS_WATER_CELL_SHIFT);
        const int64_t firstY = SosFloorShift(around.y - radius, SOS_WATER_CELL_SHIFT);
        const uint32_t count = QueryBox(water, terrain, firstX, firstY, side);
        uint64_t nearest = UINT64_MAX;
        for (uint32_t index = 0u; index < count; ++index)
        {
            const uint64_t distance = SosCellsGroundDistanceSquared(around, box[index].position);
            if (distance <= (uint64_t)(radius * radius) && distance < nearest)
            {
                nearest = distance;
            }
        }
        SosWaterSource found;
        const bool any = SosWaterFindNearest(water, terrain, around, radius, &found);
        Expect(any == (nearest != UINT64_MAX),
               "a search finds water exactly when some is in reach");
        if (!any)
        {
            continue;
        }
        Expect(SosCellsGroundDistanceSquared(around, found.position) == nearest,
               "a search finds the nearest source");
        SosWaterSource twice;
        Expect(SosWaterFindNearest(water, terrain, around, radius, &twice) &&
                   twice.cellX == found.cellX && twice.cellY == found.cellY &&
                   twice.quality == found.quality,
               "a search replays");
    }
}

static void TestEdges(const SosWater *water, const SosTerrain *terrain)
{
    SosWaterSource source;
    SosWaterInit(NULL, SEED);
    Expect(!SosWaterInCell(NULL, terrain, 0, 0, &source) &&
               !SosWaterInCell(water, NULL, 0, 0, &source) &&
               !SosWaterInCell(water, terrain, 0, 0, NULL),
           "cell lookups reject NULL");
    Expect(!SosWaterInCell(water, terrain, SOS_WATER_CELL_LIMIT, 0, &source) &&
               !SosWaterInCell(water, terrain, INT64_MAX, INT64_MAX, &source),
           "cells outside the world are dry");
    Expect(!SosWaterFindNearest(NULL, terrain, (SosPosition){0, 0, 0}, SOS_METRES(64), &source) &&
               !SosWaterFindNearest(water, NULL, (SosPosition){0, 0, 0}, SOS_METRES(64), &source) &&
               !SosWaterFindNearest(water, terrain, (SosPosition){0, 0, 0}, -1, &source) &&
               !SosWaterFindNearest(water, terrain, (SosPosition){0, 0, 0},
                                    SOS_WATER_QUERY_MAX_CELLS * SOS_WATER_CELL_UNITS, NULL),
           "searches reject bad arguments");
    Expect(SosWaterQuery(NULL, terrain, 0, 0, 0, 0, &source, 1u) == 0u &&
               SosWaterQuery(water, NULL, 0, 0, 0, 0, &source, 1u) == 0u &&
               SosWaterQuery(water, terrain, 10, 0, 0, 0, &source, 1u) == 0u &&
               SosWaterQuery(water, terrain, 0, 10, 0, 0, &source, 1u) == 0u &&
               SosWaterQuery(water, terrain, 0, 0, 0, 0, NULL, 1u) == 0u,
           "queries reject bad arguments");
    /* A box of all cells is wider than the search bound. */
    Expect(SosWaterQuery(water, terrain, 0, 0,
                         (SOS_WATER_QUERY_MAX_CELLS + 1) * SOS_WATER_CELL_UNITS, 0, &source,
                         1u) == 0u,
           "an oversized query is rejected");
    /* A NULL output with no capacity still counts the sources. */
    const uint32_t count = SosWaterQuery(water, terrain, 0, 0, SOS_WATER_CELL_UNITS - 1,
                                         SOS_WATER_CELL_UNITS - 1, NULL, 0u);
    Expect(count <= 1u, "a one-cell query holds at most one source");
}

/* Fixed points of the water cover. Every platform and compiler must place
 * exactly these sources. */
static void TestGoldenValues(const SosWater *water, const SosTerrain *terrain)
{
    const uint32_t count = QueryBox(water, terrain, -1000, 2500, 8);
    uint64_t fingerprint = count;
    for (uint32_t index = 0u; index < count; ++index)
    {
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.x);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.y);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].position.z);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)box[index].kind);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)box[index].quality);
    }
    const uint64_t expected = UINT64_C(0xA6A848546E590F94);
    if (fingerprint != expected)
    {
        fprintf(stderr, "water fingerprint: 0x%016" PRIX64 " (%u sources)\n", fingerprint, count);
        Expect(0, "the water cover matches the reference");
    }
}

int main(void)
{
    SosTerrain terrain;
    SosTerrainInit(&terrain, SEED);
    SosWater water;
    SosWaterInit(&water, SEED);
    TestDeterminism(&terrain);
    TestOnSurface(&water, &terrain);
    TestDensity(&water, &terrain);
    TestFindNearest(&water, &terrain);
    TestEdges(&water, &terrain);
    TestGoldenValues(&water, &terrain);
    return 0;
}
