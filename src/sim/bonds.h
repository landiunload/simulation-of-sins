#ifndef SOS_SIM_BONDS_H
#define SOS_SIM_BONDS_H

#include <stdbool.h>
#include <stdint.h>

/* Pair bonds.
 *
 * A bond ties two beings as a courtship, a union or a marriage. Bonds are the
 * record a culture reads: who is together, who was together, how often a pair
 * came back to each other and how each bond ended. Whether a being may hold
 * several bonds at once is not built in; the caller passes the culture's
 * `maxPartners` limit to every formation, so monogamy is one rule among many
 * and no rule is a special case.
 *
 * A bond is named by the ordered pair (lower serial, higher serial), so a pair
 * has at most one record no matter who formed it. Ended bonds are kept: the
 * history of partners is what makes a reunion, a widow or a broken tie
 * meaningful. Each being's bonds are an entry in a shared SosLinks set, in the
 * order the bonds formed, so its current and past partners are always
 * enumerable without scanning every bond. Only what callers change is stored,
 * and no work happens while time passes, so millions of idle beings cost
 * nothing. */

typedef enum SosBondKind
{
    SOS_BOND_COURTSHIP = 0,
    SOS_BOND_UNION,
    SOS_BOND_MARRIAGE,
    SOS_BOND_KIND_COUNT,
} SosBondKind;

typedef enum SosBondEnd
{
    /* The bond is still active. */
    SOS_BOND_END_NONE = 0,
    SOS_BOND_END_SEPARATION,
    SOS_BOND_END_DEATH,
    SOS_BOND_END_COUNT,
} SosBondEnd;

/* One bond, as read back for presentation and tools. `endReason` is
 * SOS_BOND_END_NONE exactly while the bond is active; `endedTick` is
 * meaningless then. `reunions` counts how often the pair formed again after
 * having ended. `first` and `second` are the ordered pair. */
typedef struct SosBond
{
    uint64_t first;
    uint64_t second;
    SosBondKind kind;
    SosBondEnd endReason;
    uint32_t reunions;
    uint64_t formedTick;
    uint64_t endedTick;
} SosBond;

typedef struct SosBonds SosBonds;

/* An empty set of bonds, or NULL when memory ran out. */
SosBonds *SosBondsCreate(void);
void SosBondsDestroy(SosBonds *bonds);

/* Forms a bond between two distinct beings. Fails when either serial is zero,
 * the two are equal, `kind` is unknown, the pair already has an active bond,
 * or either being already holds `maxPartners` active bonds. Re-forming an
 * ended pair reuses its record, keeps its history and counts one reunion; a
 * `maxPartners` of zero therefore lets no new bond form at all. On every
 * failure, including when memory runs out, the set is unchanged. */
bool SosBondsForm(SosBonds *bonds, uint64_t a, uint64_t b, SosBondKind kind, uint64_t tick,
                  uint32_t maxPartners);

/* Ends the active bond of a pair with `reason`, which must be a separation or
 * a death. Fails when the pair has no active bond or `tick` is before the bond
 * was formed. */
bool SosBondsEnd(SosBonds *bonds, uint64_t a, uint64_t b, SosBondEnd reason, uint64_t tick);

/* Ends every bond of a being that was formed at or before `tick`, for example
 * on death, and returns how many ended. Bonds formed after `tick` stay active.
 * A zero serial, an unknown reason or a being without bonds ends nothing. */
uint32_t SosBondsEndAllOf(SosBonds *bonds, uint64_t being, SosBondEnd reason, uint64_t tick);

/* The bond between two beings, active or ended. False for invalid arguments or
 * a pair that never had a bond; `outBond` is then untouched. */
bool SosBondsGet(const SosBonds *bonds, uint64_t a, uint64_t b, SosBond *outBond);

/* Writes the partners of a being, oldest bond first, up to `capacity` of them,
 * and returns how many there are, which may exceed `capacity`. `activeOnly`
 * keeps just the bonds that have not ended. A zero serial or a being without
 * bonds writes nothing. `out` may be NULL only when `capacity` is zero. */
uint32_t SosBondsPartners(const SosBonds *bonds, uint64_t being, bool activeOnly, uint64_t *out,
                          uint32_t capacity);

/* How many bonds the being has ever had, ended ones included. */
uint32_t SosBondsHistoryCount(const SosBonds *bonds, uint64_t being);

/* Folds every bond and every being's partner list into one value, independent
 * of where the set stores them and of the order it visits them, so equal
 * histories hash equally on every machine. */
uint64_t SosBondsHash(const SosBonds *bonds);

#endif
