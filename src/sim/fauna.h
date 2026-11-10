#ifndef SOS_SIM_FAUNA_H
#define SOS_SIM_FAUNA_H

#include "sim/fixed.h"
#include "sim/space.h"
#include "sim/terrain.h"

#include <stdbool.h>
#include <stdint.h>

/* Wild animals.
 *
 * In a 256 m fauna cell one herd of one species may live. Whether it lives
 * there, which species it is, where its home is and how many animals it counts
 * follow from the world seed, the cell and the ground, so the unbounded world
 * never stores a herd. Only what hunting changes is stored: the animals taken
 * from a herd are remembered until the herd has regrown them, then forgotten
 * and again indistinguishable from a herd nobody ever hunted. This is the same
 * sparse layer over a function as the plant cover.
 *
 * A herd roams between hashed waypoints of the day and the next around its
 * home, always on the ground. Its position at a tick and how many animals are
 * alive then are computed from the tick alone, so idle herds cost nothing and
 * a coarse step over a hunt equals many fine ones. */

#define SOS_FAUNA_CELL_SHIFT 18u
#define SOS_FAUNA_CELL_UNITS (INT64_C(1) << SOS_FAUNA_CELL_SHIFT)
/* Searches and queries cover at most this many cells per side. */
#define SOS_FAUNA_QUERY_MAX_CELLS 1024
/* A herd keeps its daily waypoints within this distance of home, and its home
 * this far from the cell's edges, so it never leaves its cell. */
#define SOS_FAUNA_HERD_RANGE SOS_METRES(64)
/* Densities are drawn out of this many shares; their sum with a matching
 * ground is the chance that a cell holds a herd. */
#define SOS_FAUNA_CHANCE 65536u

typedef enum SosFaunaSpecies
{
    SOS_FAUNA_NONE = 0,
    SOS_FAUNA_RABBIT,
    SOS_FAUNA_DEER,
    SOS_FAUNA_BOAR,
    SOS_FAUNA_AUROCHS,
    SOS_FAUNA_WOLF,
    SOS_FAUNA_BEAR,
    SOS_FAUNA_SPECIES_COUNT,
} SosFaunaSpecies;

/* The fixed properties of a species. Where a field could vary with the world
 * seed or a being, a new accessor should return it instead of this table. */
typedef struct SosFaunaSpeciesInfo
{
    /* The ground a herd of this species needs at its home. */
    SosMaterial ground;
    uint32_t density;
    uint32_t minimumSize;
    uint32_t maximumSize;
    /* Food one animal gives a hunter. */
    uint32_t meat;
    /* How dangerous one animal is, in Q16; zero for harmless game. */
    SosQ16 danger;
    /* A person nearer than this scares the herd away. */
    int64_t fleeDistance;
    /* Ticks for one taken animal to come back. */
    int64_t regrowth;
} SosFaunaSpeciesInfo;

typedef struct SosHerd
{
    /* The fauna cell, a stable name for the herd. */
    int64_t cellX;
    int64_t cellY;
    /* The middle of its territory, on the ground. */
    SosPosition home;
    SosFaunaSpecies species;
    /* How many animals the world function puts there. */
    uint32_t size;
    /* How many are alive at the queried tick. */
    uint32_t alive;
} SosHerd;

typedef struct SosFauna SosFauna;

SosFauna *SosFaunaCreate(uint64_t worldSeed);
void SosFaunaDestroy(SosFauna *fauna);

/* The fixed properties of a species; false for SOS_FAUNA_NONE, an unknown
 * species or a NULL output. */
bool SosFaunaSpeciesInfoGet(SosFaunaSpecies species, SosFaunaSpeciesInfo *outInfo);

/* The herd living in a cell at `tick`, with the animals already taken
 * subtracted. False for an empty cell, a NULL argument or a cell outside the
 * world's coordinate range. A herd hunted to nothing still lives and returns;
 * its `alive` is zero until an animal grows back. */
bool SosFaunaInCell(const SosFauna *fauna, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                    uint64_t tick, SosHerd *outHerd);

/* Where a herd is at `tick`: between the waypoints of the day and the next,
 * standing on the ground. It moves continuously across a day boundary. A NULL
 * herd or terrain, or a cell outside the world's coordinate range, gives the
 * zero position. */
SosPosition SosFaunaHerdPosition(const SosHerd *herd, const SosTerrain *terrain, uint64_t tick);

/* The nearest herd with an animal alive within `radius` of a point on the
 * ground plane, `speciesFilter` selecting one species or SOS_FAUNA_NONE for
 * any. Of equally near herds the first in a fixed cell order wins, so every
 * machine picks the same one. False when there is none, or on a NULL argument,
 * an unknown species, a negative radius or one larger than the query range. */
bool SosFaunaFindNearest(const SosFauna *fauna, const SosTerrain *terrain, SosPosition around,
                         int64_t radius, SosFaunaSpecies speciesFilter, uint64_t tick,
                         SosHerd *outHerd);

/* Herds whose cells overlap the box [minX, maxX] x [minY, maxY] at `tick`.
 * Writes up to `capacity` of them, X fastest, and returns how many there are,
 * which may exceed `capacity`. A reversed box, a NULL argument, or a box wider
 * than SOS_FAUNA_QUERY_MAX_CELLS cells returns zero. */
uint32_t SosFaunaQuery(const SosFauna *fauna, const SosTerrain *terrain, int64_t minX, int64_t minY,
                       int64_t maxX, int64_t maxY, uint64_t tick, SosHerd *outHerds,
                       uint32_t capacity);

/* Takes up to `count` animals from the herd in a cell and returns how many
 * were taken. The loss regrows over the species' regrowth time. An empty cell,
 * a herd hunted to nothing, or a NULL argument gives zero. When memory runs out
 * the animals are still taken but not remembered, so the world loses detail,
 * never animals. */
uint32_t SosFaunaHunt(SosFauna *fauna, const SosTerrain *terrain, int64_t cellX, int64_t cellY,
                      uint64_t tick, uint32_t count);

/* Folds the state at `tick` into one value. A herd that has regrown its
 * animals counts as never hunted, so equal worlds give equal hashes however
 * long ago they were hunted; a herd regrowing halfway hashes as its remaining
 * animals. */
uint64_t SosFaunaHash(const SosFauna *fauna, uint64_t tick);

#endif
