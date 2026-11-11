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

/* What beings and the god changed in the ground (terrain_edits.h). */
typedef struct SosTerrainEdits SosTerrainEdits;

typedef struct SosTerrain
{
    uint64_t seed;
    /* NULL for untouched ground; otherwise every query includes the edits. */
    const SosTerrainEdits *edits;
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

/* Height of the ground surface at a point, in world units: where a walker
 * stands. Natural caves do not change it; dug ground does. */
int64_t SosTerrainSurfaceZ(const SosTerrain *terrain, int64_t x, int64_t y);
SosTerrainSurface SosTerrainSurfaceAt(const SosTerrain *terrain, int64_t x, int64_t y);

/* One sample at a world position, with caves and edits. Near edits the value
 * is interpolated between lattice samples, which is where edits live. */
SosTerrainSample SosTerrainSampleAt(const SosTerrain *terrain, SosPosition position);

/* The sample at a lattice cell, with caves and edits. */
SosTerrainSample SosTerrainSampleCell(const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                                      int64_t cellZ);

/* Rays stop marching at most this far, and step at least this much. */
#define SOS_TERRAIN_RAY_MAX_DISTANCE SOS_METRES(200000)
#define SOS_TERRAIN_RAY_MIN_STEP (SOS_METRES(1) / 8)

/* The first point where a ray enters solid ground, edits included: from
 * `origin` along `direction` (any non-zero length) for at most `maxDistance`
 * world units. The march steps by the sample's distance to the surface,
 * halved for slopes, then bisects the crossing to about a centimetre; ground
 * thinner than a minimum step may be missed. False when the ray stays in the
 * air or the input is invalid. Deterministic: a being's line of sight can use
 * it as well as a cursor. */
bool SosTerrainRaycast(const SosTerrain *terrain, SosPosition origin, SosPosition direction,
                       int64_t maxDistance, SosPosition *outHit);

/* Fills countX * countY * countZ samples, X fastest, then Y, then Z. False on
 * an invalid request, which leaves the outputs untouched. Thread-safe: the
 * terrain is read-only. */
bool SosTerrainSampleGrid(const SosTerrain *terrain, const SosTerrainGrid *grid,
                          SosTerrainSample *samples);

#endif
