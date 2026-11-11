#include "sim/terrain_edits.h"

#include "sim/random.h"
#include "sim/table.h"

#include <stddef.h>
#include <stdlib.h>

#define CHUNK_SAMPLES                                                                              \
    (SOS_TERRAIN_EDITS_CHUNK_CELLS * SOS_TERRAIN_EDITS_CHUNK_CELLS * SOS_TERRAIN_EDITS_CHUNK_CELLS)
#define CHUNK_MASK (SOS_TERRAIN_EDITS_CHUNK_CELLS - 1)
#define CELL ((int64_t)SOS_TERRAIN_CELL_UNITS)
/* Samples this far outside the sphere still take its distance, so the dug
 * surface is smooth; deeper ground keeps the function's values, which only
 * differ in how solid it is, never in where the surface lies. */
#define DIG_BAND (2 * CELL)
/* Lattice cells along one axis of the largest dig. */
#define DIG_MAX_CELLS ((2 * (SOS_TERRAIN_DIG_MAX_RADIUS + DIG_BAND)) / CELL + 3)
/* Densities are clamped like the terrain function's. */
#define DENSITY_LIMIT (INT64_C(1) << 30)

typedef struct Chunk
{
    uint64_t edited[CHUNK_SAMPLES / 64];
    int32_t density[CHUNK_SAMPLES];
    uint8_t material[CHUNK_SAMPLES];
} Chunk;

/* The vertical extent of the edited chunks above one chunk column. */
typedef struct Column
{
    int64_t lowest;
    int64_t highest;
} Column;

struct SosTerrainEdits
{
    /* Keyed by chunk X and chunk Y and Z packed into 32 bits each. */
    SosTable *chunks;
    /* Keyed by chunk X and chunk Y. */
    SosTable *columns;
};

SosTerrainEdits *SosTerrainEditsCreate(void)
{
    SosTerrainEdits *edits = calloc(1u, sizeof(*edits));
    if (edits == NULL)
    {
        return NULL;
    }
    edits->chunks = SosTableCreate(sizeof(Chunk));
    edits->columns = SosTableCreate(sizeof(Column));
    if (edits->chunks == NULL || edits->columns == NULL)
    {
        SosTerrainEditsDestroy(edits);
        return NULL;
    }
    return edits;
}

void SosTerrainEditsDestroy(SosTerrainEdits *edits)
{
    if (edits != NULL)
    {
        SosTableDestroy(edits->chunks);
        SosTableDestroy(edits->columns);
        free(edits);
    }
}

uint32_t SosTerrainEditsChunkCount(const SosTerrainEdits *edits)
{
    return edits != NULL ? SosTableCount(edits->chunks) : 0u;
}

static bool ChunkInRange(int64_t chunk)
{
    return chunk >= -SOS_TERRAIN_EDITS_MAX_CHUNK && chunk < SOS_TERRAIN_EDITS_MAX_CHUNK;
}

static SosTableKey ChunkKey(int64_t chunkX, int64_t chunkY, int64_t chunkZ)
{
    return (SosTableKey){
        .first = (uint64_t)chunkX,
        .second = ((uint64_t)(uint32_t)chunkY << 32u) | (uint64_t)(uint32_t)chunkZ,
    };
}

static SosTableKey ColumnKey(int64_t chunkX, int64_t chunkY)
{
    return (SosTableKey){.first = (uint64_t)chunkX, .second = (uint64_t)chunkY};
}

static uint32_t SampleIndex(int64_t cellX, int64_t cellY, int64_t cellZ)
{
    return (uint32_t)((cellX & CHUNK_MASK) | ((cellY & CHUNK_MASK) << 3) |
                      ((cellZ & CHUNK_MASK) << 6));
}

bool SosTerrainEditsSample(const SosTerrainEdits *edits, int64_t cellX, int64_t cellY,
                           int64_t cellZ, SosTerrainSample *outSample)
{
    if (edits == NULL || SosTableCount(edits->chunks) == 0u)
    {
        return false;
    }
    const int64_t chunkY = SosFloorShift(cellY, SOS_TERRAIN_EDITS_CHUNK_SHIFT);
    const int64_t chunkZ = SosFloorShift(cellZ, SOS_TERRAIN_EDITS_CHUNK_SHIFT);
    if (!ChunkInRange(chunkY) || !ChunkInRange(chunkZ))
    {
        return false;
    }
    const Chunk *chunk =
        SosTableFind(edits->chunks,
                     ChunkKey(SosFloorShift(cellX, SOS_TERRAIN_EDITS_CHUNK_SHIFT), chunkY, chunkZ));
    if (chunk == NULL)
    {
        return false;
    }
    const uint32_t index = SampleIndex(cellX, cellY, cellZ);
    if ((chunk->edited[index / 64u] & (UINT64_C(1) << (index % 64u))) == 0u)
    {
        return false;
    }
    if (outSample != NULL)
    {
        *outSample = (SosTerrainSample){.density = chunk->density[index],
                                        .material = chunk->material[index]};
    }
    return true;
}

bool SosTerrainEditsColumn(const SosTerrainEdits *edits, int64_t cellX, int64_t cellY,
                           int64_t *outLowestChunkZ, int64_t *outHighestChunkZ)
{
    if (edits == NULL || SosTableCount(edits->columns) == 0u)
    {
        return false;
    }
    const Column *column = SosTableFind(
        edits->columns, ColumnKey(SosFloorShift(cellX, SOS_TERRAIN_EDITS_CHUNK_SHIFT),
                                  SosFloorShift(cellY, SOS_TERRAIN_EDITS_CHUNK_SHIFT)));
    if (column == NULL)
    {
        return false;
    }
    if (outLowestChunkZ != NULL)
    {
        *outLowestChunkZ = column->lowest;
    }
    if (outHighestChunkZ != NULL)
    {
        *outHighestChunkZ = column->highest;
    }
    return true;
}

/* The lattice cells a dig may change, or false when they leave the range. */
static bool DigCells(SosPosition center, int64_t radius, SosPosition *outMin, SosPosition *outMax)
{
    const int64_t reach = radius + DIG_BAND;
    /* Half the chunk range, so every cell of the dig has a 32-bit chunk. */
    const int64_t limit = SOS_TERRAIN_EDITS_MAX_CHUNK * SOS_TERRAIN_EDITS_CHUNK_CELLS * CELL / 2;
    if (center.y <= -limit || center.y >= limit || center.z <= -limit || center.z >= limit ||
        center.x <= INT64_MIN / 2 || center.x >= INT64_MAX / 2)
    {
        return false;
    }
    *outMin = (SosPosition){SosFloorShift(center.x - reach, SOS_TERRAIN_CELL_SHIFT),
                            SosFloorShift(center.y - reach, SOS_TERRAIN_CELL_SHIFT),
                            SosFloorShift(center.z - reach, SOS_TERRAIN_CELL_SHIFT)};
    *outMax = (SosPosition){SosFloorShift(center.x + reach, SOS_TERRAIN_CELL_SHIFT) + 1,
                            SosFloorShift(center.y + reach, SOS_TERRAIN_CELL_SHIFT) + 1,
                            SosFloorShift(center.z + reach, SOS_TERRAIN_CELL_SHIFT) + 1};
    return true;
}

/* Makes room for every chunk and column the cells may need, so the dig
 * either completes or changes nothing. */
static bool ReserveFor(SosTerrainEdits *edits, SosPosition minCell, SosPosition maxCell)
{
    const unsigned shift = SOS_TERRAIN_EDITS_CHUNK_SHIFT;
    uint32_t newChunks = 0u;
    uint32_t newColumns = 0u;
    for (int64_t chunkY = SosFloorShift(minCell.y, shift);
         chunkY <= SosFloorShift(maxCell.y, shift); ++chunkY)
    {
        for (int64_t chunkX = SosFloorShift(minCell.x, shift);
             chunkX <= SosFloorShift(maxCell.x, shift); ++chunkX)
        {
            newColumns += SosTableFind(edits->columns, ColumnKey(chunkX, chunkY)) == NULL ? 1u : 0u;
            for (int64_t chunkZ = SosFloorShift(minCell.z, shift);
                 chunkZ <= SosFloorShift(maxCell.z, shift); ++chunkZ)
            {
                newChunks +=
                    SosTableFind(edits->chunks, ChunkKey(chunkX, chunkY, chunkZ)) == NULL ? 1u : 0u;
            }
        }
    }
    return SosTableReserve(edits->chunks, SosTableCount(edits->chunks) + newChunks) &&
           SosTableReserve(edits->columns, SosTableCount(edits->columns) + newColumns);
}

static void Store(SosTerrainEdits *edits, int64_t cellX, int64_t cellY, int64_t cellZ,
                  SosTerrainSample sample)
{
    const unsigned shift = SOS_TERRAIN_EDITS_CHUNK_SHIFT;
    const int64_t chunkX = SosFloorShift(cellX, shift);
    const int64_t chunkY = SosFloorShift(cellY, shift);
    const int64_t chunkZ = SosFloorShift(cellZ, shift);
    /* Both inserts were reserved: they cannot fail. */
    bool created = false;
    Column *column = SosTableInsert(edits->columns, ColumnKey(chunkX, chunkY), &created);
    Chunk *chunk = SosTableInsert(edits->chunks, ChunkKey(chunkX, chunkY, chunkZ), NULL);
    if (column == NULL || chunk == NULL)
    {
        return;
    }
    if (created)
    {
        *column = (Column){.lowest = chunkZ, .highest = chunkZ};
    }
    column->lowest = chunkZ < column->lowest ? chunkZ : column->lowest;
    column->highest = chunkZ > column->highest ? chunkZ : column->highest;
    const uint32_t index = SampleIndex(cellX, cellY, cellZ);
    chunk->edited[index / 64u] |= UINT64_C(1) << (index % 64u);
    chunk->density[index] = sample.density;
    chunk->material[index] = sample.material;
}

static void Include(SosTerrainBox *box, SosPosition point)
{
    box->min.x = point.x < box->min.x ? point.x : box->min.x;
    box->min.y = point.y < box->min.y ? point.y : box->min.y;
    box->min.z = point.z < box->min.z ? point.z : box->min.z;
    box->max.x = point.x > box->max.x ? point.x : box->max.x;
    box->max.y = point.y > box->max.y ? point.y : box->max.y;
    box->max.z = point.z > box->max.z ? point.z : box->max.z;
}

uint32_t SosTerrainEditsDig(SosTerrainEdits *edits, const SosTerrain *terrain, SosPosition center,
                            int64_t radius, SosTerrainBox *outChanged)
{
    SosTerrainBox changed = {.min = {INT64_MAX, INT64_MAX, INT64_MAX},
                             .max = {INT64_MIN, INT64_MIN, INT64_MIN}};
    if (outChanged != NULL)
    {
        *outChanged = changed;
    }
    radius = radius < SOS_TERRAIN_DIG_MIN_RADIUS   ? SOS_TERRAIN_DIG_MIN_RADIUS
             : radius > SOS_TERRAIN_DIG_MAX_RADIUS ? SOS_TERRAIN_DIG_MAX_RADIUS
                                                   : radius;
    SosPosition minCell;
    SosPosition maxCell;
    if (edits == NULL || terrain == NULL || terrain->edits != edits ||
        !DigCells(center, radius, &minCell, &maxCell) || !ReserveFor(edits, minCell, maxCell))
    {
        return 0u;
    }
    const int64_t reach = radius + DIG_BAND;
    const uint32_t countZ = (uint32_t)(maxCell.z - minCell.z + 1);
    SosTerrainSample column[DIG_MAX_CELLS];
    uint32_t count = 0u;
    for (int64_t cellY = minCell.y; cellY <= maxCell.y; ++cellY)
    {
        for (int64_t cellX = minCell.x; cellX <= maxCell.x; ++cellX)
        {
            const int64_t dx = cellX * CELL - center.x;
            const int64_t dy = cellY * CELL - center.y;
            const int64_t across = dx * dx + dy * dy;
            /* One grid per column: the height field once, not per level. */
            const SosTerrainGrid grid = {.cellX = cellX,
                                         .cellY = cellY,
                                         .cellZ = minCell.z,
                                         .stride = 1u,
                                         .countX = 1u,
                                         .countY = 1u,
                                         .countZ = countZ};
            if (across > reach * reach || !SosTerrainSampleGrid(terrain, &grid, column))
            {
                continue;
            }
            for (uint32_t level = 0u; level < countZ; ++level)
            {
                const int64_t cellZ = minCell.z + (int64_t)level;
                const int64_t dz = cellZ * CELL - center.z;
                const int64_t distance = (int64_t)SosIntegerSqrt((uint64_t)(across + dz * dz));
                const SosTerrainSample current = column[level];
                const int64_t carved = distance - radius;
                /* Ground that was solid before any dig takes the smallest of
                 * the spheres dug into it, whatever their order; air the
                 * function made stays as it was. */
                if (distance > reach || carved >= current.density ||
                    (current.density <= 0 &&
                     !SosTerrainEditsSample(edits, cellX, cellY, cellZ, NULL)))
                {
                    continue;
                }
                const int64_t density = carved < -DENSITY_LIMIT ? -DENSITY_LIMIT : carved;
                Store(edits, cellX, cellY, cellZ,
                      (SosTerrainSample){.density = (int32_t)density,
                                         .material = density > 0 ? current.material
                                                                 : (uint8_t)SOS_MATERIAL_AIR});
                Include(&changed, (SosPosition){cellX * CELL, cellY * CELL, cellZ * CELL});
                ++count;
            }
        }
    }
    if (outChanged != NULL)
    {
        *outChanged = changed;
    }
    return count;
}

static uint64_t ChunkHash(SosTableKey key, const void *record, void *context)
{
    (void)context;
    const Chunk *chunk = record;
    uint64_t hash = SosHashCombine(key.first, key.second);
    for (uint32_t index = 0u; index < CHUNK_SAMPLES; ++index)
    {
        if ((chunk->edited[index / 64u] & (UINT64_C(1) << (index % 64u))) != 0u)
        {
            hash = SosHashCombine(hash, ((uint64_t)index << 40u) ^
                                            ((uint64_t)chunk->material[index] << 32u) ^
                                            (uint64_t)(uint32_t)chunk->density[index]);
        }
    }
    return hash;
}

uint64_t SosTerrainEditsHash(const SosTerrainEdits *edits)
{
    return edits != NULL ? SosTableFold(edits->chunks, ChunkHash, NULL) : 0u;
}
