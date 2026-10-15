#include "sim/water.h"

#include "sim/cells.h"
#include "sim/random.h"

/* Chances are out of this many cells. */
#define CHANCE_DEN (UINT32_C(1) << 16)
/* A hollow counts only once its floor lies this far below the average of its
 * neighbours: half a metre clears the fine noise of the height field. */
#define MIN_RELIEF (SOS_METRES(1) / 2)
/* Chance out of CHANCE_DEN for the shallowest hollow that counts at all. */
#define BASE_CHANCE 2048u
/* Each world unit of extra depth adds this much chance, so a hollow two
 * metres deeper than the threshold is a strong candidate. */
#define RELIEF_SLOPE 8
/* No hollow is more than half likely: water also needs a place to collect. */
#define RELIEF_CHANCE_MAX 32768
/* One low-ground source in four is a spring; the rest are ponds. */
#define SPRING_ONE_IN 4u
/* Quality varies within a quarter of full, so each kind keeps its own band. */
#define QUALITY_SPAN (SOS_Q16_ONE / 4)
#define NEIGHBOUR_COUNT 4

/* Water is cleanest at a spring, clean in a pond and least clean in a lake,
 * which has had the longest to gather silt. */
static const SosQ16 baseQuality[SOS_WATER_KIND_COUNT] = {
    [SOS_WATER_SPRING] = SOS_Q16_ONE * 3 / 4,
    [SOS_WATER_POND] = SOS_Q16_ONE / 2,
    [SOS_WATER_LAKE] = SOS_Q16_ONE / 4,
};

void SosWaterInit(SosWater *water, uint64_t worldSeed)
{
    if (water != NULL)
    {
        water->seed = SosHashCombine(worldSeed, UINT64_C(0x7761746572)); /* "water" */
    }
}

static bool CellWithinLimit(int64_t cellX, int64_t cellY)
{
    return cellX > -SOS_WATER_CELL_LIMIT && cellX < SOS_WATER_CELL_LIMIT &&
           cellY > -SOS_WATER_CELL_LIMIT && cellY < SOS_WATER_CELL_LIMIT;
}

/* How far the ground at a point lies below its surroundings: positive in a
 * hollow, near zero on a slope (the neighbours' heights average out) and
 * negative on a hill. */
static int64_t Relief(const SosTerrain *terrain, int64_t x, int64_t y, int64_t centreZ)
{
    const int64_t gap = SOS_WATER_CELL_UNITS;
    const int64_t sum =
        SosTerrainSurfaceZ(terrain, x - gap, y) + SosTerrainSurfaceZ(terrain, x + gap, y) +
        SosTerrainSurfaceZ(terrain, x, y - gap) + SosTerrainSurfaceZ(terrain, x, y + gap);
    return sum / NEIGHBOUR_COUNT - centreZ;
}

/* The source the world function puts into a cell, which is the same on every
 * machine. */
static bool Generate(const SosWater *water, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                     SosWaterSource *outSource)
{
    if (!CellWithinLimit(cellX, cellY))
    {
        return false;
    }
    const int64_t x = cellX * SOS_WATER_CELL_UNITS + SOS_WATER_CELL_UNITS / 2;
    const int64_t y = cellY * SOS_WATER_CELL_UNITS + SOS_WATER_CELL_UNITS / 2;
    const SosTerrainSurface surface = SosTerrainSurfaceAt(terrain, x, y);
    if (surface.material == SOS_MATERIAL_SNOW)
    {
        return false;
    }
    /* A cell's own draws follow from its coordinates alone, so how a cell is
     * generated never depends on which cells were looked at before it. */
    SosRandom random =
        SosRandomStream(water->seed, SosHashCombine((uint64_t)cellX, (uint64_t)cellY));
    SosWaterKind kind;
    if (surface.z < SOS_WATER_LEVEL)
    {
        kind = SOS_WATER_LAKE;
    }
    else
    {
        const int64_t relief = Relief(terrain, x, y, surface.z);
        if (relief < MIN_RELIEF)
        {
            return false;
        }
        int64_t extra = (relief - MIN_RELIEF) * RELIEF_SLOPE;
        if (extra > RELIEF_CHANCE_MAX)
        {
            extra = RELIEF_CHANCE_MAX;
        }
        if (!SosRandomChance(&random, BASE_CHANCE + (uint32_t)extra, CHANCE_DEN))
        {
            return false;
        }
        kind = SosRandomBelow(&random, SPRING_ONE_IN) == 0u ? SOS_WATER_SPRING : SOS_WATER_POND;
    }
    const SosQ16 quality =
        (SosQ16)(baseQuality[kind] + (SosQ16)SosRandomBelow(&random, (uint32_t)QUALITY_SPAN));
    *outSource = (SosWaterSource){
        .cellX = cellX,
        .cellY = cellY,
        .position = {x, y, surface.z},
        .kind = kind,
        .quality = quality,
    };
    return true;
}

bool SosWaterInCell(const SosWater *water, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                    SosWaterSource *outSource)
{
    if (water == NULL || terrain == NULL || outSource == NULL)
    {
        return false;
    }
    return Generate(water, terrain, cellX, cellY, outSource);
}

/* State of one nearest-source search, shared with its visitor. */
typedef struct NearestWater
{
    const SosWater *water;
    const SosTerrain *terrain;
    SosPosition around;
    uint64_t radiusSquared;
    /* The squared distance of the best source found, UINT64_MAX for none. */
    uint64_t best;
    SosWaterSource source;
} NearestWater;

static void VisitWater(int64_t cellX, int64_t cellY, void *context)
{
    NearestWater *search = context;
    SosWaterSource source;
    if (!Generate(search->water, search->terrain, cellX, cellY, &source))
    {
        return;
    }
    const uint64_t distance = SosCellsGroundDistanceSquared(search->around, source.position);
    if (distance <= search->radiusSquared && distance < search->best)
    {
        search->best = distance;
        search->source = source;
    }
}

bool SosWaterFindNearest(const SosWater *water, const SosTerrain *terrain, SosPosition around,
                         int64_t radius, SosWaterSource *outSource)
{
    if (water == NULL || terrain == NULL || outSource == NULL || radius < 0)
    {
        return false;
    }
    NearestWater search = {.water = water,
                           .terrain = terrain,
                           .around = around,
                           .radiusSquared = SosCellsSquared((uint64_t)radius),
                           .best = UINT64_MAX};
    if (!SosCellsVisitRings(around, radius, SOS_WATER_CELL_SHIFT, SOS_WATER_QUERY_MAX_CELLS,
                            &search.best, VisitWater, &search) ||
        search.best == UINT64_MAX)
    {
        return false;
    }
    *outSource = search.source;
    return true;
}

/* State of one box query, shared with its visitor. */
typedef struct WaterCollect
{
    const SosWater *water;
    const SosTerrain *terrain;
    SosWaterSource *outSources;
    uint32_t capacity;
    uint32_t found;
} WaterCollect;

static void CollectWater(int64_t cellX, int64_t cellY, void *context)
{
    WaterCollect *collect = context;
    SosWaterSource source;
    if (Generate(collect->water, collect->terrain, cellX, cellY, &source))
    {
        if (collect->found < collect->capacity)
        {
            collect->outSources[collect->found] = source;
        }
        collect->found += 1u;
    }
}

uint32_t SosWaterQuery(const SosWater *water, const SosTerrain *terrain, int64_t minX, int64_t minY,
                       int64_t maxX, int64_t maxY, SosWaterSource *outSources, uint32_t capacity)
{
    if (water == NULL || terrain == NULL || (outSources == NULL && capacity != 0u))
    {
        return 0u;
    }
    WaterCollect collect = {.water = water,
                            .terrain = terrain,
                            .outSources = outSources,
                            .capacity = capacity,
                            .found = 0u};
    SosCellsVisitBox(minX, minY, maxX, maxY, SOS_WATER_CELL_SHIFT, SOS_WATER_QUERY_MAX_CELLS,
                     CollectWater, &collect);
    return collect.found;
}
