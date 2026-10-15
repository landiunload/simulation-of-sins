#include "sim/terrain.h"

#include "sim/random.h"

#include <stddef.h>

/* Fixed point with 16 fractional bits: noise values lie in [-1, 1]. */
#define Q16 INT64_C(65536)
#define DENSITY_LIMIT (INT64_C(1) << 30)
/* Lattice cells within +-2^53 (2^62 world units) stay inside the 64-bit unit
 * range after multiplying by the cell size and adding a full grid. */
#define CELL_LIMIT (INT64_C(1) << 53)

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
#define CAVE_THRESHOLD (Q16 / 12)
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

static int64_t Lattice(uint64_t seed, int64_t x, int64_t y, int64_t z)
{
    const uint64_t hash =
        SosHashCombine(SosHashCombine(SosHashCombine(seed, (uint64_t)x), (uint64_t)y), (uint64_t)z);
    /* 17 bits mapped to [-1, 1). */
    return (int64_t)(hash >> 47) - Q16;
}

static int64_t Fraction(int64_t coordinate, unsigned shift)
{
    const uint64_t remainder = SosFloorRemainder(coordinate, shift);
    return shift >= 16u ? (int64_t)(remainder >> (shift - 16u))
                        : (int64_t)(remainder << (16u - shift));
}

static int64_t Smooth(int64_t t)
{
    /* 3t^2 - 2t^3 in Q16; truncating division is exact and portable. */
    const int64_t squared = t * t / Q16;
    return squared * (3 * Q16 - 2 * t) / Q16;
}

static int64_t Lerp(int64_t from, int64_t to, int64_t t)
{
    return from + (to - from) * t / Q16;
}

static int64_t Noise2(uint64_t seed, int64_t x, int64_t y, unsigned shift)
{
    const int64_t ix = SosFloorShift(x, shift);
    const int64_t iy = SosFloorShift(y, shift);
    const int64_t sx = Smooth(Fraction(x, shift));
    const int64_t sy = Smooth(Fraction(y, shift));
    const int64_t bottom = Lerp(Lattice(seed, ix, iy, 0), Lattice(seed, ix + 1, iy, 0), sx);
    const int64_t top = Lerp(Lattice(seed, ix, iy + 1, 0), Lattice(seed, ix + 1, iy + 1, 0), sx);
    return Lerp(bottom, top, sy);
}

static int64_t Noise3(uint64_t seed, int64_t x, int64_t y, int64_t z, unsigned shift)
{
    const int64_t ix = SosFloorShift(x, shift);
    const int64_t iy = SosFloorShift(y, shift);
    const int64_t iz = SosFloorShift(z, shift);
    const int64_t sx = Smooth(Fraction(x, shift));
    const int64_t sy = Smooth(Fraction(y, shift));
    const int64_t sz = Smooth(Fraction(z, shift));
    int64_t layers[2];
    for (int64_t dz = 0; dz < 2; ++dz)
    {
        const int64_t bottom =
            Lerp(Lattice(seed, ix, iy, iz + dz), Lattice(seed, ix + 1, iy, iz + dz), sx);
        const int64_t top =
            Lerp(Lattice(seed, ix, iy + 1, iz + dz), Lattice(seed, ix + 1, iy + 1, iz + dz), sx);
        layers[dz] = Lerp(bottom, top, sy);
    }
    return Lerp(layers[0], layers[1], sz);
}

static int64_t Clamp(int64_t value, int64_t limit)
{
    return value > limit ? limit : (value < -limit ? -limit : value);
}

void SosTerrainInit(SosTerrain *terrain, uint64_t worldSeed)
{
    terrain->seed = SosHashCombine(worldSeed, UINT64_C(0x7465727261696e)); /* "terrain" */
}

int64_t SosTerrainSurfaceZ(const SosTerrain *terrain, int64_t x, int64_t y)
{
    int64_t height = 0;
    for (size_t index = 0u; index < sizeof(heightOctaves) / sizeof(heightOctaves[0]); ++index)
    {
        const Octave *octave = &heightOctaves[index];
        const uint64_t seed = SosHashCombine(terrain->seed, SALT_HEIGHT + index);
        height += octave->amplitude * Noise2(seed, x, y, octave->shift) / Q16;
    }
    return height;
}

/* Negative inside a cave tunnel. */
static int64_t CaveDensity(const SosTerrain *terrain, int64_t x, int64_t y, int64_t z)
{
    const int64_t large =
        Noise3(SosHashCombine(terrain->seed, SALT_CAVE_LARGE), x, y, z, CAVE_LARGE_SHIFT);
    const int64_t small =
        Noise3(SosHashCombine(terrain->seed, SALT_CAVE_SMALL), x, y, z, CAVE_SMALL_SHIFT);
    int64_t value = large + small / 2;
    value = value < 0 ? -value : value;
    return (value - CAVE_THRESHOLD) * CAVE_SCALE / Q16;
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

SosTerrainSample SosTerrainSampleAt(const SosTerrain *terrain, SosPosition position)
{
    const int64_t surfaceZ = SosTerrainSurfaceZ(terrain, position.x, position.y);
    return Sample(terrain, surfaceZ, position.x, position.y, position.z, true,
                  SOS_TERRAIN_CELL_UNITS);
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
            const int64_t surfaceZ = SosTerrainSurfaceZ(terrain, x, y);
            SosTerrainSample *cell = samples + (size_t)row * grid->countX + column;
            for (uint32_t level = 0u; level < grid->countZ; ++level)
            {
                const int64_t z = originZ + (int64_t)level * step;
                cell[(size_t)level * layer] = Sample(terrain, surfaceZ, x, y, z, caves, step);
            }
        }
    }
    return true;
}
