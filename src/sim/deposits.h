#ifndef SOS_SIM_DEPOSITS_H
#define SOS_SIM_DEPOSITS_H

#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/space.h"
#include "sim/terrain.h"

#include <stdbool.h>
#include <stdint.h>

/* Natural materials.
 *
 * Flint, stone, clay, ochre, salt, obsidian, timber and reeds are part of the
 * world function: whether a 32 m cell holds a deposit, its kind, richness and
 * place in the cell follow from the world seed, the cell and the terrain
 * beneath it, so the unbounded world never stores them. Only what beings take
 * is stored: a worked deposit is remembered as an amount left at a tick, and
 * fades from memory once timber or reeds have grown back. This is the plants
 * pattern, a sparse layer of changes over a function. Minerals never renew;
 * timber and reeds renew slowly, so a camp that exhausts its woods must move.
 *
 * The cell is 32 m, four times the plant cell: a quarry, a clay pit or a
 * stand of trees is a landmark, not a bush. Searching and querying cover at
 * most SOS_DEPOSIT_QUERY_MAX_CELLS cells per side, about 16 km. */

#define SOS_DEPOSIT_CELL_SHIFT 15u
#define SOS_DEPOSIT_CELL_UNITS (INT64_C(1) << SOS_DEPOSIT_CELL_SHIFT)
#define SOS_DEPOSIT_QUERY_MAX_CELLS 1024u

/* A felled tree grows back over a year, a cut reed bed over a month; the
 * interval is what makes a settlement's neighbours matter. */
#define SOS_DEPOSIT_TIMBER_RENEWAL SOS_TICKS_PER_YEAR
#define SOS_DEPOSIT_REED_RENEWAL (30 * SOS_TICKS_PER_DAY)

typedef enum SosDepositKind
{
    SOS_DEPOSIT_NONE = 0,
    /* Hills: flint and stone outcrops, ochre in the soil, rare obsidian. */
    SOS_DEPOSIT_FLINT,
    SOS_DEPOSIT_STONE,
    /* Low ground around the sand line: clay, salt on the flats. */
    SOS_DEPOSIT_CLAY,
    SOS_DEPOSIT_OCHRE,
    SOS_DEPOSIT_SALT,
    SOS_DEPOSIT_OBSIDIAN,
    /* Living materials on grass and near the shore. */
    SOS_DEPOSIT_TIMBER,
    SOS_DEPOSIT_REED,
    SOS_DEPOSIT_KIND_COUNT,
} SosDepositKind;

/* A bit per kind, for the kind filter of a search. */
#define SOS_DEPOSIT_KIND_MASK(kind) (UINT32_C(1) << (uint32_t)(kind))
#define SOS_DEPOSIT_KIND_ALL ((UINT32_C(1) << SOS_DEPOSIT_KIND_COUNT) - 1u)

typedef struct SosDeposit
{
    /* The deposit cell, a stable name for the deposit. */
    int64_t cellX;
    int64_t cellY;
    /* On the ground surface. */
    SosPosition position;
    SosDepositKind kind;
    /* Quality in (0, SOS_Q16_ONE], fixed for the deposit; it scales how much
     * material the deposit holds. */
    SosQ16 richness;
    /* Amount left to extract at the queried tick, zero when worked out. */
    uint32_t amount;
} SosDeposit;

typedef struct SosDeposits SosDeposits;

/* A deposits layer over the world `worldSeed`, or NULL when memory ran out. */
SosDeposits *SosDepositsCreate(uint64_t worldSeed);
void SosDepositsDestroy(SosDeposits *deposits);

/* The deposit in a cell at `tick`; false for an empty cell. A worked-out
 * deposit is still a deposit, reported with an amount of zero. */
bool SosDepositsInCell(const SosDeposits *deposits, const SosTerrain *terrain, int64_t cellX,
                       int64_t cellY, uint64_t tick, SosDeposit *outDeposit);

/* The nearest deposit of a kind in `kindMask` that still holds material,
 * within `radius` of a point on the ground plane. Of equally near deposits
 * the first in a fixed cell order wins, so every machine picks the same one.
 * False when there is none, when the mask is zero, or when the radius is
 * negative or wider than SOS_DEPOSIT_QUERY_MAX_CELLS cells. */
bool SosDepositsFindNearest(const SosDeposits *deposits, const SosTerrain *terrain,
                            SosPosition around, int64_t radius, uint64_t tick, uint32_t kindMask,
                            SosDeposit *outDeposit);

/* Takes up to `amount` material from the deposit in a cell at `tick` and
 * returns how much was actually taken; zero for an empty cell, a worked-out
 * deposit or a zero request. Timber and reeds grow back over their renewal
 * time; minerals never do. When memory runs out the material is still taken
 * but not remembered: the world loses detail, never material. */
uint32_t SosDepositsExtract(SosDeposits *deposits, const SosTerrain *terrain, int64_t cellX,
                            int64_t cellY, uint64_t tick, uint32_t amount);

/* Deposits whose cells overlap the box [minX, maxX] x [minY, maxY]. Writes up
 * to `capacity` of them, X fastest, and returns how many there are, which may
 * exceed `capacity`. A box wider than SOS_DEPOSIT_QUERY_MAX_CELLS cells
 * returns zero. */
uint32_t SosDepositsQuery(const SosDeposits *deposits, const SosTerrain *terrain, int64_t minX,
                          int64_t minY, int64_t maxX, int64_t maxY, uint64_t tick,
                          SosDeposit *outDeposits, uint32_t capacity);

/* Folds the state at `tick` into one value. A deposit grown back to its full
 * amount counts as never touched, so equal worlds give equal hashes however
 * long ago their deposits were worked. */
uint64_t SosDepositsHash(const SosDeposits *deposits, uint64_t tick);

#endif
