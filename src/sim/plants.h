#ifndef SOS_SIM_PLANTS_H
#define SOS_SIM_PLANTS_H

#include "sim/space.h"
#include "sim/terrain.h"

#include <stdbool.h>
#include <stdint.h>

/* Wild plants.
 *
 * The plant cover is part of the world function: whether a 4 m plant cell
 * holds a plant, its species and its place in the cell follow from the world
 * seed and the cell, so the unbounded world never stores it. Only what beings
 * change is stored: a harvested plant is remembered until it bears food
 * again. Every changeable part of the world follows this pattern, a sparse
 * layer of changes over a function. */

#define SOS_PLANT_CELL_SHIFT 12u
#define SOS_PLANT_CELL_UNITS (INT64_C(1) << SOS_PLANT_CELL_SHIFT)
/* Searches and queries cover at most this many cells per side. */
#define SOS_PLANT_QUERY_MAX_CELLS 1024

typedef enum SosPlantSpecies
{
    SOS_PLANT_NONE = 0,
    SOS_PLANT_BERRY_BUSH,
    SOS_PLANT_SPECIES_COUNT,
} SosPlantSpecies;

typedef struct SosPlant
{
    /* The plant cell, a stable name for the plant. */
    int64_t cellX;
    int64_t cellY;
    /* On the ground surface. */
    SosPosition position;
    SosPlantSpecies species;
    /* Whether the plant bears food at the queried tick. */
    bool ripe;
} SosPlant;

typedef struct SosPlants SosPlants;

SosPlants *SosPlantsCreate(uint64_t worldSeed);
void SosPlantsDestroy(SosPlants *plants);

/* The plant growing in a cell at `tick`; false for an empty cell. */
bool SosPlantsInCell(const SosPlants *plants, const SosTerrain *terrain, int64_t cellX,
                     int64_t cellY, uint64_t tick, SosPlant *outPlant);

/* The nearest plant bearing food within `radius` of a point on the ground
 * plane. Of equally near plants the first in a fixed cell order wins, so
 * every machine picks the same one. False when there is none. */
bool SosPlantsFindRipe(const SosPlants *plants, const SosTerrain *terrain, SosPosition around,
                       int64_t radius, uint64_t tick, SosPlant *outPlant);

/* Takes the food of the plant in a cell; it grows again over the species'
 * regrowth time. False when the cell holds no plant or its food is gone. When
 * memory runs out the food is still taken but the plant is not remembered as
 * bare: the world loses detail, never food. */
bool SosPlantsHarvest(SosPlants *plants, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                      uint64_t tick);

/* Plants whose cells overlap the box [minX, maxX] x [minY, maxY]. Writes up to
 * `capacity` of them, X fastest, and returns how many there are, which may
 * exceed `capacity`. A box wider than SOS_PLANT_QUERY_MAX_CELLS cells returns
 * zero. */
uint32_t SosPlantsQuery(const SosPlants *plants, const SosTerrain *terrain, int64_t minX,
                        int64_t minY, int64_t maxX, int64_t maxY, uint64_t tick,
                        SosPlant *outPlants, uint32_t capacity);

/* Folds the state at `tick` into one value. Plants that bear food again
 * count as never harvested, so equal worlds give equal hashes however long
 * ago their plants were harvested. */
uint64_t SosPlantsHash(const SosPlants *plants, uint64_t tick);

#endif
