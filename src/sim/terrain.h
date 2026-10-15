#ifndef SOS_SIM_TERRAIN_H
#define SOS_SIM_TERRAIN_H

#include "sim/space.h"

#include <stdbool.h>
#include <stdint.h>

/* Volumetric procedural terrain.
 *
 * The ground is a density field: positive inside solid matter, negative in
 * air, zero on the surface. It is a pure function of the world seed computed
 * in integers, so every machine generates the same world anywhere in the
 * unbounded space without storing it. Hills come from a height field and
 * caves from three-dimensional noise below it. The field is sampled on a
 * 0.5 m lattice, the resolution at which the world will later be dug and
 * built; edits will be stored as a sparse layer over this function. */

#define SOS_TERRAIN_CELL_SHIFT 9u
#define SOS_TERRAIN_CELL_UNITS (1 << SOS_TERRAIN_CELL_SHIFT)

typedef enum SosMaterial
{
    SOS_MATERIAL_AIR = 0,
    SOS_MATERIAL_GRASS,
    SOS_MATERIAL_DIRT,
    SOS_MATERIAL_STONE,
    SOS_MATERIAL_SAND,
    SOS_MATERIAL_SNOW,
    SOS_MATERIAL_COUNT,
} SosMaterial;

typedef struct SosTerrain
{
    uint64_t seed;
} SosTerrain;

typedef struct SosTerrainSample
{
    /* Signed distance-like value in world units, clamped to +-2^30. */
    int32_t density;
    uint8_t material;
} SosTerrainSample;

/* The ground surface at a point: where a walker stands and what grows there. */
typedef struct SosTerrainSurface
{
    int64_t z;
    /* The top layer: grass, sand or snow. */
    uint8_t material;
} SosTerrainSurface;

/* A box of lattice samples. `stride` is the distance between samples in
 * cells, a power of two up to 2^20: far terrain is sampled coarsely. Caves are
 * left out when `stride` exceeds SOS_TERRAIN_CAVE_MAX_STRIDE, because they are
 * smaller than such a sample spacing. */
typedef struct SosTerrainGrid
{
    int64_t cellX;
    int64_t cellY;
    int64_t cellZ;
    uint32_t stride;
    uint32_t countX;
    uint32_t countY;
    uint32_t countZ;
} SosTerrainGrid;

#define SOS_TERRAIN_CAVE_MAX_STRIDE 4u
#define SOS_TERRAIN_GRID_MAX_SAMPLES (1u << 20)

void SosTerrainInit(SosTerrain *terrain, uint64_t worldSeed);

/* Height of the ground surface at a point, in world units. Caves do not
 * change it: this is where a walker stands. */
int64_t SosTerrainSurfaceZ(const SosTerrain *terrain, int64_t x, int64_t y);
SosTerrainSurface SosTerrainSurfaceAt(const SosTerrain *terrain, int64_t x, int64_t y);

/* One sample at a world position, with caves. */
SosTerrainSample SosTerrainSampleAt(const SosTerrain *terrain, SosPosition position);

/* Fills countX * countY * countZ samples, X fastest, then Y, then Z. False on
 * an invalid request, which leaves the outputs untouched. Thread-safe: the
 * terrain is read-only. */
bool SosTerrainSampleGrid(const SosTerrain *terrain, const SosTerrainGrid *grid,
                          SosTerrainSample *samples);

#endif
