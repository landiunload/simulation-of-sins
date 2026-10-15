#ifndef SOS_SIM_WATER_H
#define SOS_SIM_WATER_H

#include "sim/fixed.h"
#include "sim/space.h"
#include "sim/terrain.h"

#include <stdbool.h>
#include <stdint.h>

/* Drinking water.
 *
 * Like the plant cover, water is part of the world function: whether a 64 m
 * cell holds a spring, a pond or a lake, where in it the water stands and how
 * good it is to drink all follow from the world seed and the cell, so the
 * unbounded world never stores them. Water never runs out, so nothing about it
 * changes and there is no state to remember: a SosWater is only the world
 * seed. The module reads the terrain, whose height and material decide where
 * water collects: deep ground lies under a named level and becomes a lake,
 * ground that is lower than its surroundings gathers a spring or a pond, and
 * snow never holds water. A thirsty being asks for the nearest source and
 * walks to it. */

/* Cells are 64 m across: a body of water is a handful of them, small enough
 * to walk around and large enough to spot from a distance. */
#define SOS_WATER_CELL_SHIFT 16u
#define SOS_WATER_CELL_UNITS (INT64_C(1) << SOS_WATER_CELL_SHIFT)
/* Searches and queries cover at most this many cells per side. */
#define SOS_WATER_QUERY_MAX_CELLS 1024
/* Ground below this level is under water, so its cell is a lake. Sand begins
 * at -20 m (terrain), so a lake bed is sand or grass and never snow. */
#define SOS_WATER_LEVEL SOS_METRES(-25)
/* Cell coordinates stay within this distance of the origin so a cell's centre
 * and its four neighbours stay inside the signed 64-bit world range. */
#define SOS_WATER_CELL_LIMIT ((INT64_MAX / 2) / SOS_WATER_CELL_UNITS)

typedef enum SosWaterKind
{
    SOS_WATER_NONE = 0,
    /* Water welling from the ground, the cleanest. */
    SOS_WATER_SPRING,
    /* Standing water in a small hollow. */
    SOS_WATER_POND,
    /* One cell of a body of water below SOS_WATER_LEVEL. */
    SOS_WATER_LAKE,
    SOS_WATER_KIND_COUNT,
} SosWaterKind;

typedef struct SosWaterSource
{
    /* The water cell, a stable name for the source. */
    int64_t cellX;
    int64_t cellY;
    /* On the ground surface. */
    SosPosition position;
    SosWaterKind kind;
    /* How good the water is to drink, in [SOS_Q16_ONE / 4, SOS_Q16_ONE):
     * springs are cleanest, lakes the most turbid. */
    SosQ16 quality;
} SosWaterSource;

typedef struct SosWater
{
    uint64_t seed;
} SosWater;

/* Prepares the water function of a world. The same seed always gives the same
 * water everywhere; the module keeps no other state. A NULL `water` is
 * ignored. */
void SosWaterInit(SosWater *water, uint64_t worldSeed);

/* The water source in a cell, or false for a dry cell. A NULL argument or a
 * cell beyond SOS_WATER_CELL_LIMIT also returns false. */
bool SosWaterInCell(const SosWater *water, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                    SosWaterSource *outSource);

/* The nearest source within `radius` of a point on the ground plane. Of
 * equally near sources the first in a fixed cell order wins, so every machine
 * picks the same one. False when there is none, when an argument is NULL, when
 * `radius` is negative, or when it reaches further than the search's
 * SOS_WATER_QUERY_MAX_CELLS bound. */
bool SosWaterFindNearest(const SosWater *water, const SosTerrain *terrain, SosPosition around,
                         int64_t radius, SosWaterSource *outSource);

/* Sources whose cells overlap the box [minX, maxX] x [minY, maxY]. Writes up
 * to `capacity` of them, X fastest, and returns how many there are, which may
 * exceed `capacity`. Zero for a NULL argument, a reversed box or a box wider
 * than SOS_WATER_QUERY_MAX_CELLS cells; a NULL `outSources` with non-zero
 * `capacity` writes nothing and returns zero. */
uint32_t SosWaterQuery(const SosWater *water, const SosTerrain *terrain, int64_t minX, int64_t minY,
                       int64_t maxX, int64_t maxY, SosWaterSource *outSources, uint32_t capacity);

#endif
