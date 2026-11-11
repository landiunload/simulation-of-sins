#include "sim/terrain.h"

#include "sim/fixed.h"
#include "sim/noise.h"
#include "sim/random.h"
#include "sim/terrain_edits.h"

#include <stddef.h>

#define DENSITY_LIMIT (INT64_C(1) << 30)
/* Lattice cells within +-2^53 (2^62 world units) stay inside the 64-bit unit
 * range after multiplying by the cell size and adding a full grid. */
#define CELL_LIMIT (INT64_C(1) << 53)
#define CELL ((int64_t)SOS_TERRAIN_CELL_UNITS)
/* Rays move in steps of RAY_UNIT per world unit, so a unit direction keeps
 * sixteen bits of precision; their components stay below RAY_COMPONENT_LIMIT
 * while the length is measured, so its square fits 64 bits. */
#define RAY_UNIT (INT64_C(1) << 16)
#define RAY_COMPONENT_LIMIT (INT64_C(1) << 30)
#define RAY_MAX_STEPS 8192
/* The bisection stops within about a centimetre. */
#define RAY_PRECISION INT64_C(8)

typedef struct Octave
{
    /* Wavelength 2^shift world units. */
    unsigned shift;
    int64_t amplitude;
} Octave;

/* Hills from 16 km down to 4 m. */
static const Octave heightOctaves[] = {
    {24u, SOS_METRES(260)}, {22u, SOS_METRES(120)}, {20u, SOS_METRES(45)},    {18u, SOS_METRES(14)},
    {16u, SOS_METRES(4)},   {14u, SOS_METRES(1)},   {12u, SOS_METRES(1) / 4},
};

/* Caves: tunnels of a few metres winding over tens of metres. */
#define CAVE_LARGE_SHIFT 15u
#define CAVE_SMALL_SHIFT 13u
#define CAVE_THRESHOLD (SOS_Q16_ONE / 12)
#define CAVE_SCALE SOS_METRES(8)
#define CAVE_MIN_DEPTH SOS_METRES(3)

#define SNOW_LINE SOS_METRES(300)
#define SAND_LINE SOS_METRES(-20)
#define TOPSOIL_DEPTH (SOS_METRES(1) / 2)
#define DIRT_DEPTH SOS_METRES(3)

enum
{
    SALT_HEIGHT = 0x100,
    SALT_CAVE_LARGE = 0x200,
    SALT_CAVE_SMALL = 0x201,
};

static int64_t Clamp(int64_t value, int64_t limit)
{
    return value > limit ? limit : (value < -limit ? -limit : value);
}

void SosTerrainInit(SosTerrain *terrain, uint64_t worldSeed)
{
    terrain->seed = SosHashCombine(worldSeed, UINT64_C(0x7465727261696e)); /* "terrain" */
    terrain->edits = NULL;
}

/* The height field of the untouched ground. */
static int64_t FunctionSurfaceZ(const SosTerrain *terrain, int64_t x, int64_t y)
{
    int64_t height = 0;
    for (size_t index = 0u; index < sizeof(heightOctaves) / sizeof(heightOctaves[0]); ++index)
    {
        const Octave *octave = &heightOctaves[index];
        const uint64_t seed = SosHashCombine(terrain->seed, SALT_HEIGHT + index);
        height += octave->amplitude * SosNoise2(seed, x, y, octave->shift) / SOS_Q16_ONE;
    }
    return height;
}

/* Negative inside a cave tunnel. */
static int64_t CaveDensity(const SosTerrain *terrain, int64_t x, int64_t y, int64_t z)
{
    const int64_t large =
        SosNoise3(SosHashCombine(terrain->seed, SALT_CAVE_LARGE), x, y, z, CAVE_LARGE_SHIFT);
    const int64_t small =
        SosNoise3(SosHashCombine(terrain->seed, SALT_CAVE_SMALL), x, y, z, CAVE_SMALL_SHIFT);
    int64_t value = large + small / 2;
    value = value < 0 ? -value : value;
    return (value - CAVE_THRESHOLD) * CAVE_SCALE / SOS_Q16_ONE;
}

/* `spacing` is the distance between neighbouring samples in world units. A
 * sample stands for the ground around it, so the top layer is at least two
 * spacings thick: a coarse view of a meadow is still a meadow, not the stone
 * a few metres down. */
static SosTerrainSample Sample(const SosTerrain *terrain, int64_t surfaceZ, int64_t x, int64_t y,
                               int64_t z, bool caves, int64_t spacing)
{
    const int64_t topsoil = 2 * spacing > TOPSOIL_DEPTH ? 2 * spacing : TOPSOIL_DEPTH;
    const int64_t depth = surfaceZ - z;
    int64_t density = depth;
    bool cave = false;
    if (caves && depth > CAVE_MIN_DEPTH)
    {
        const int64_t carved = CaveDensity(terrain, x, y, z);
        if (carved < density)
        {
            density = carved;
            cave = true;
        }
    }
    SosTerrainSample sample = {.density = (int32_t)Clamp(density, DENSITY_LIMIT)};
    /* Cave walls are bare stone; elsewhere the layers follow the depth. */
    if (density <= 0)
    {
        sample.material = SOS_MATERIAL_AIR;
    }
    else if (!cave && depth < topsoil)
    {
        sample.material = surfaceZ > SNOW_LINE   ? SOS_MATERIAL_SNOW
                          : surfaceZ < SAND_LINE ? SOS_MATERIAL_SAND
                                                 : SOS_MATERIAL_GRASS;
    }
    else if (!cave && depth < topsoil + DIRT_DEPTH && surfaceZ <= SNOW_LINE)
    {
        sample.material = surfaceZ < SAND_LINE ? SOS_MATERIAL_SAND : SOS_MATERIAL_DIRT;
    }
    else
    {
        sample.material = SOS_MATERIAL_STONE;
    }
    return sample;
}

/* A lattice sample: the edit standing there, or the function's own sample.
 * `surfaceZ` is the function's height at the cell's column. */
static SosTerrainSample CellSample(const SosTerrain *terrain, int64_t surfaceZ, int64_t cellX,
                                   int64_t cellY, int64_t cellZ, bool caves)
{
    SosTerrainSample edited;
    if (SosTerrainEditsSample(terrain->edits, cellX, cellY, cellZ, &edited))
    {
        return edited;
    }
    return Sample(terrain, surfaceZ, cellX * CELL, cellY * CELL, cellZ * CELL, caves, CELL);
}

static bool CellIsValid(int64_t cell)
{
    return cell > -CELL_LIMIT && cell < CELL_LIMIT;
}

SosTerrainSample SosTerrainSampleCell(const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                                      int64_t cellZ)
{
    if (terrain == NULL || !CellIsValid(cellX) || !CellIsValid(cellY) || !CellIsValid(cellZ))
    {
        return (SosTerrainSample){.density = 0, .material = SOS_MATERIAL_AIR};
    }
    return CellSample(terrain, FunctionSurfaceZ(terrain, cellX * CELL, cellY * CELL), cellX, cellY,
                      cellZ, true);
}

/* The lattice cells around a point on the ground plane and the function's
 * height at each of their four columns. */
typedef struct Corners
{
    int64_t cellX;
    int64_t cellY;
    /* Position within the cell, in world units [0, CELL). */
    int64_t offsetX;
    int64_t offsetY;
    int64_t surfaceZ[4];
} Corners;

static Corners CornersAround(const SosTerrain *terrain, int64_t x, int64_t y)
{
    Corners corners = {
        .cellX = SosFloorShift(x, SOS_TERRAIN_CELL_SHIFT),
        .cellY = SosFloorShift(y, SOS_TERRAIN_CELL_SHIFT),
        .offsetX = (int64_t)SosFloorRemainder(x, SOS_TERRAIN_CELL_SHIFT),
        .offsetY = (int64_t)SosFloorRemainder(y, SOS_TERRAIN_CELL_SHIFT),
    };
    for (int64_t corner = 0; corner < 4; ++corner)
    {
        corners.surfaceZ[corner] = FunctionSurfaceZ(terrain, (corners.cellX + (corner & 1)) * CELL,
                                                    (corners.cellY + (corner >> 1)) * CELL);
    }
    return corners;
}

/* The edited lattice levels over the four columns around a cell; false when
 * none of them was touched, which keeps untouched ground on the exact
 * function. */
static bool EditsAround(const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                        int64_t *outLowestCell, int64_t *outHighestCell)
{
    bool edited = false;
    int64_t lowest = INT64_MAX;
    int64_t highest = INT64_MIN;
    for (int64_t corner = 0; terrain->edits != NULL && corner < 4; ++corner)
    {
        int64_t low = 0;
        int64_t high = 0;
        if (SosTerrainEditsColumn(terrain->edits, cellX + (corner & 1), cellY + (corner >> 1), &low,
                                  &high))
        {
            edited = true;
            lowest = low < lowest ? low : lowest;
            highest = high > highest ? high : highest;
        }
    }
    *outLowestCell = edited ? lowest * SOS_TERRAIN_EDITS_CHUNK_CELLS : 0;
    *outHighestCell = edited ? (highest + 1) * SOS_TERRAIN_EDITS_CHUNK_CELLS - 1 : 0;
    return edited;
}

/* The sample at a lattice level between the four columns, interpolated in X
 * and Y; the material is that of the nearest column. */
static SosTerrainSample Bilinear(const SosTerrain *terrain, const Corners *corners, int64_t cellZ,
                                 bool caves)
{
    SosTerrainSample samples[4];
    for (int64_t corner = 0; corner < 4; ++corner)
    {
        samples[corner] =
            CellSample(terrain, corners->surfaceZ[corner], corners->cellX + (corner & 1),
                       corners->cellY + (corner >> 1), cellZ, caves);
    }
    const int64_t fx = corners->offsetX;
    const int64_t fy = corners->offsetY;
    const int64_t bottom =
        (int64_t)samples[0].density * (CELL - fx) + (int64_t)samples[1].density * fx;
    const int64_t top =
        (int64_t)samples[2].density * (CELL - fx) + (int64_t)samples[3].density * fx;
    const int64_t nearest = (fx >= CELL / 2 ? 1 : 0) + (fy >= CELL / 2 ? 2 : 0);
    return (SosTerrainSample){.density =
                                  (int32_t)((bottom * (CELL - fy) + top * fy) / (CELL * CELL)),
                              .material = samples[nearest].material};
}

/* The top of the solid ground over edited columns: scans the lattice down
 * from above every edit and the function's surface to the first solid level,
 * then places the surface where the density crosses zero. */
static SosTerrainSurface EditedSurface(const SosTerrain *terrain, const Corners *corners,
                                       int64_t lowestCell, int64_t highestCell)
{
    int64_t top = highestCell + 1;
    int64_t bottom = lowestCell - 1;
    for (int64_t corner = 0; corner < 4; ++corner)
    {
        const int64_t level = SosFloorShift(corners->surfaceZ[corner], SOS_TERRAIN_CELL_SHIFT);
        top = level + 1 > top ? level + 1 : top;
        bottom = level - 1 < bottom ? level - 1 : bottom;
    }
    SosTerrainSample above = Bilinear(terrain, corners, top, false);
    for (int64_t level = top - 1; level >= bottom; --level)
    {
        const SosTerrainSample below = Bilinear(terrain, corners, level, false);
        if (below.density > 0)
        {
            const int64_t span = (int64_t)below.density - (above.density < 0 ? above.density : 0);
            return (SosTerrainSurface){.z = level * CELL + CELL * below.density / span,
                                       .material = below.material};
        }
        above = below;
    }
    return (SosTerrainSurface){.z = bottom * CELL, .material = SOS_MATERIAL_STONE};
}

int64_t SosTerrainSurfaceZ(const SosTerrain *terrain, int64_t x, int64_t y)
{
    return SosTerrainSurfaceAt(terrain, x, y).z;
}

SosTerrainSurface SosTerrainSurfaceAt(const SosTerrain *terrain, int64_t x, int64_t y)
{
    int64_t lowestCell = 0;
    int64_t highestCell = 0;
    if (EditsAround(terrain, SosFloorShift(x, SOS_TERRAIN_CELL_SHIFT),
                    SosFloorShift(y, SOS_TERRAIN_CELL_SHIFT), &lowestCell, &highestCell))
    {
        const Corners corners = CornersAround(terrain, x, y);
        return EditedSurface(terrain, &corners, lowestCell, highestCell);
    }
    const int64_t surfaceZ = FunctionSurfaceZ(terrain, x, y);
    /* The topmost solid unit holds the top layer. */
    const SosTerrainSample top = Sample(terrain, surfaceZ, x, y, surfaceZ - 1, false, CELL);
    return (SosTerrainSurface){.z = surfaceZ, .material = top.material};
}

SosTerrainSample SosTerrainSampleAt(const SosTerrain *terrain, SosPosition position)
{
    const int64_t cellZ = SosFloorShift(position.z, SOS_TERRAIN_CELL_SHIFT);
    int64_t lowestCell = 0;
    int64_t highestCell = 0;
    if (EditsAround(terrain, SosFloorShift(position.x, SOS_TERRAIN_CELL_SHIFT),
                    SosFloorShift(position.y, SOS_TERRAIN_CELL_SHIFT), &lowestCell, &highestCell) &&
        cellZ + 1 >= lowestCell && cellZ <= highestCell)
    {
        /* Edits live on the lattice: interpolate between its levels. */
        const Corners corners = CornersAround(terrain, position.x, position.y);
        const SosTerrainSample below = Bilinear(terrain, &corners, cellZ, true);
        const SosTerrainSample above = Bilinear(terrain, &corners, cellZ + 1, true);
        const int64_t fz = (int64_t)SosFloorRemainder(position.z, SOS_TERRAIN_CELL_SHIFT);
        return (SosTerrainSample){
            .density =
                (int32_t)(((int64_t)below.density * (CELL - fz) + (int64_t)above.density * fz) /
                          CELL),
            .material = fz >= CELL / 2 ? above.material : below.material,
        };
    }
    const int64_t surfaceZ = FunctionSurfaceZ(terrain, position.x, position.y);
    return Sample(terrain, surfaceZ, position.x, position.y, position.z, true, CELL);
}

static int64_t RayPoint(int64_t origin, int64_t unit, int64_t distance)
{
    return origin + unit * distance / RAY_UNIT;
}

static SosPosition RayAt(SosPosition origin, const int64_t unit[3], int64_t distance)
{
    return (SosPosition){RayPoint(origin.x, unit[0], distance),
                         RayPoint(origin.y, unit[1], distance),
                         RayPoint(origin.z, unit[2], distance)};
}

bool SosTerrainRaycast(const SosTerrain *terrain, SosPosition origin, SosPosition direction,
                       int64_t maxDistance, SosPosition *outHit)
{
    const int64_t positionLimit = INT64_C(1) << 61;
    if (terrain == NULL || outHit == NULL || maxDistance <= 0 || origin.x <= -positionLimit ||
        origin.x >= positionLimit || origin.y <= -positionLimit || origin.y >= positionLimit ||
        origin.z <= -positionLimit || origin.z >= positionLimit)
    {
        return false;
    }
    maxDistance =
        maxDistance < SOS_TERRAIN_RAY_MAX_DISTANCE ? maxDistance : SOS_TERRAIN_RAY_MAX_DISTANCE;
    /* Shrink the direction until its squared length fits 64 bits, then scale
     * it to RAY_UNIT. */
    int64_t components[3] = {direction.x, direction.y, direction.z};
    for (int pass = 0; pass < 64; ++pass)
    {
        const bool small =
            components[0] > -RAY_COMPONENT_LIMIT && components[0] < RAY_COMPONENT_LIMIT &&
            components[1] > -RAY_COMPONENT_LIMIT && components[1] < RAY_COMPONENT_LIMIT &&
            components[2] > -RAY_COMPONENT_LIMIT && components[2] < RAY_COMPONENT_LIMIT;
        if (small)
        {
            break;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            components[axis] /= 2;
        }
    }
    const uint64_t length = SosIntegerSqrt((uint64_t)(components[0] * components[0]) +
                                           (uint64_t)(components[1] * components[1]) +
                                           (uint64_t)(components[2] * components[2]));
    if (length == 0u)
    {
        return false;
    }
    const int64_t unit[3] = {components[0] * RAY_UNIT / (int64_t)length,
                             components[1] * RAY_UNIT / (int64_t)length,
                             components[2] * RAY_UNIT / (int64_t)length};
    int64_t travelled = 0;
    int64_t inAir = -1;
    for (int step = 0; step < RAY_MAX_STEPS && travelled <= maxDistance; ++step)
    {
        const SosTerrainSample sample = SosTerrainSampleAt(terrain, RayAt(origin, unit, travelled));
        if (sample.density > 0)
        {
            if (inAir < 0)
            {
                *outHit = origin;
                return true;
            }
            /* Air at inAir, ground at travelled: bisect the crossing. */
            int64_t solid = travelled;
            while (solid - inAir > RAY_PRECISION)
            {
                const int64_t middle = inAir + (solid - inAir) / 2;
                if (SosTerrainSampleAt(terrain, RayAt(origin, unit, middle)).density > 0)
                {
                    solid = middle;
                }
                else
                {
                    inAir = middle;
                }
            }
            *outHit = RayAt(origin, unit, solid);
            return true;
        }
        inAir = travelled;
        /* The density is the height above ground; slopes make it longer than
         * the true distance, so only half of it is taken. */
        const int64_t stride = -(int64_t)sample.density / 2;
        travelled += stride > SOS_TERRAIN_RAY_MIN_STEP ? stride : SOS_TERRAIN_RAY_MIN_STEP;
    }
    return false;
}

static bool CellRangeIsValid(int64_t first, uint32_t count, uint32_t stride)
{
    const int64_t span = (int64_t)count * (int64_t)stride;
    return first > -CELL_LIMIT && first < CELL_LIMIT && first + span < CELL_LIMIT;
}

bool SosTerrainSampleGrid(const SosTerrain *terrain, const SosTerrainGrid *grid,
                          SosTerrainSample *samples)
{
    if (terrain == NULL || grid == NULL || samples == NULL || grid->stride == 0u ||
        grid->stride > (1u << 20) || (grid->stride & (grid->stride - 1u)) != 0u ||
        grid->countX == 0u || grid->countY == 0u || grid->countZ == 0u ||
        (uint64_t)grid->countX * grid->countY * grid->countZ > SOS_TERRAIN_GRID_MAX_SAMPLES ||
        !CellRangeIsValid(grid->cellX, grid->countX, grid->stride) ||
        !CellRangeIsValid(grid->cellY, grid->countY, grid->stride) ||
        !CellRangeIsValid(grid->cellZ, grid->countZ, grid->stride))
    {
        return false;
    }
    const bool caves = grid->stride <= SOS_TERRAIN_CAVE_MAX_STRIDE;
    const int64_t step = (int64_t)grid->stride * SOS_TERRAIN_CELL_UNITS;
    const int64_t originX = grid->cellX * SOS_TERRAIN_CELL_UNITS;
    const int64_t originY = grid->cellY * SOS_TERRAIN_CELL_UNITS;
    const int64_t originZ = grid->cellZ * SOS_TERRAIN_CELL_UNITS;
    const size_t layer = (size_t)grid->countX * grid->countY;
    for (uint32_t row = 0u; row < grid->countY; ++row)
    {
        const int64_t y = originY + (int64_t)row * step;
        for (uint32_t column = 0u; column < grid->countX; ++column)
        {
            const int64_t x = originX + (int64_t)column * step;
            const int64_t surfaceZ = FunctionSurfaceZ(terrain, x, y);
            const int64_t cellX = grid->cellX + (int64_t)column * grid->stride;
            const int64_t cellY = grid->cellY + (int64_t)row * grid->stride;
            int64_t lowestChunk = 0;
            int64_t highestChunk = 0;
            const bool edited =
                SosTerrainEditsColumn(terrain->edits, cellX, cellY, &lowestChunk, &highestChunk);
            SosTerrainSample *cell = samples + (size_t)row * grid->countX + column;
            for (uint32_t level = 0u; level < grid->countZ; ++level)
            {
                const int64_t z = originZ + (int64_t)level * step;
                const int64_t cellZ = grid->cellZ + (int64_t)level * grid->stride;
                SosTerrainSample *out = &cell[(size_t)level * layer];
                const bool inEdits = edited &&
                                     cellZ >= lowestChunk * SOS_TERRAIN_EDITS_CHUNK_CELLS &&
                                     cellZ < (highestChunk + 1) * SOS_TERRAIN_EDITS_CHUNK_CELLS;
                if (!inEdits || !SosTerrainEditsSample(terrain->edits, cellX, cellY, cellZ, out))
                {
                    *out = Sample(terrain, surfaceZ, x, y, z, caves, step);
                }
            }
        }
    }
    return true;
}
