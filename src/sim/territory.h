#ifndef SOS_SIM_TERRITORY_H
#define SOS_SIM_TERRITORY_H

#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/space.h"

#include <stdbool.h>
#include <stdint.h>

/* Claimed land.
 *
 * Groups stake out land by marking it: each 64 m territory cell holds up to
 * SOS_TERRITORY_CLAIMS claims, one per group, each with the strength of the
 * claim and the tick it was last renewed. Only marked cells are stored; the
 * rest of the unbounded world is unclaimed, so this is a sparse layer over
 * empty land, like the harvested plants are over the plant cover. Strength
 * fades with a half-life of about a year unless the group passes through
 * again, and it follows from the elapsed time alone, so a claim decays the
 * same whether it is read once a year later or every tick. */

#define SOS_TERRITORY_CELL_SHIFT 16u
#define SOS_TERRITORY_CELL_UNITS (INT64_C(1) << SOS_TERRITORY_CELL_SHIFT)
/* Claims a single cell remembers; a fifth group must first outlast one of the
 * four it finds. */
#define SOS_TERRITORY_CLAIMS 4u
/* A mark touches at most this many cells, so one call stays cheap however
 * large a radius it is handed. */
#define SOS_TERRITORY_MARK_MAX_CELLS 1024u
/* Strength is a Q16 fraction and saturates here, so repeated marks of one
 * group cannot overflow it. */
#define SOS_TERRITORY_MAX_STRENGTH (4 * SOS_Q16_ONE)
/* The faintest claim that still owns its cell. */
#define SOS_TERRITORY_OWNER_THRESHOLD (SOS_Q16_ONE / 16)
/* Two claims are a contest when the weaker holds at least this much of the
 * stronger. */
#define SOS_TERRITORY_CONTEST_RATIO (SOS_Q16_ONE / 2)
/* A claim that decayed below this is nothing left to keep. */
#define SOS_TERRITORY_MIN_STRENGTH 1
/* Strength halves about once a year, as land nobody walks is forgotten. */
#define SOS_TERRITORY_HALF_LIFE SOS_TICKS_PER_YEAR

typedef struct SosTerritory SosTerritory;

/* An empty territory, nobody having marked any land. NULL when memory ran
 * out. */
SosTerritory *SosTerritoryCreate(void);
void SosTerritoryDestroy(SosTerritory *territory);

/* Strengthens `group`'s claim on every cell whose square lies within `radius`
 * of `position`, adding `amount` to each and renewing it at `tick`. A cell
 * already holding SOS_TERRITORY_CLAIMS other groups lets the newcomer in only
 * when the weakest of them is weaker than `amount`, replacing it. Returns how
 * many cells changed: zero for a NULL territory, group zero, a negative
 * radius, a non-positive amount, a radius whose cell square exceeds
 * SOS_TERRITORY_MARK_MAX_CELLS, or a position so near the world's edge that
 * the radius overflows it. Memory running out stops the mark after the cells
 * already changed, leaving the territory valid. */
uint32_t SosTerritoryMark(SosTerritory *territory, uint64_t group, SosPosition position,
                          int64_t radius, uint64_t tick, SosQ16 amount);

/* The group owning the cell holding `position` at `tick`: its strongest claim
 * strictly above SOS_TERRITORY_OWNER_THRESHOLD, a tie going to the lower group
 * id. Writes the group and its decayed strength when `outGroup` and
 * `outStrength` are not NULL. False for a NULL territory, an unclaimed cell or
 * a cell whose every claim has faded to the threshold. */
bool SosTerritoryOwner(const SosTerritory *territory, SosPosition position, uint64_t tick,
                       uint64_t *outGroup, SosQ16 *outStrength);

/* Whether two groups hold the cell at `position` at `tick` within
 * SOS_TERRITORY_CONTEST_RATIO of each other, so neither can be said to win it.
 * False for a NULL territory, an unclaimed cell or a single-claim cell. */
bool SosTerritoryContested(const SosTerritory *territory, SosPosition position, uint64_t tick);

/* Gives up every claim of `group`, in every cell: a full scan of the claims
 * table, so it costs O(cells) however few claims the group holds. Returns how
 * many claims went, forgetting the cells they leave empty. Zero for a NULL
 * territory or group zero. */
uint32_t SosTerritoryAbandon(SosTerritory *territory, uint64_t group);

/* Drops the claims that have decayed to nothing at `tick`, forgetting the
 * cells they leave empty, and returns how many went. A full scan of the
 * claims table. Zero for a NULL territory. */
uint32_t SosTerritoryPrune(SosTerritory *territory, uint64_t tick);

/* Folds the claims into one value at `tick`, seeing them decayed just as
 * SosTerritoryOwner does, so a territory that has faded to nothing hashes like
 * an empty one. Order-independent: the same claims give the same value however
 * the table laid them out. Zero for a NULL territory. */
uint64_t SosTerritoryHash(const SosTerritory *territory, uint64_t tick);

#endif
