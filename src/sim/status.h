#ifndef SOS_SIM_STATUS_H
#define SOS_SIM_STATUS_H

#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/table.h"

#include <stdbool.h>
#include <stdint.h>

/* Social standing.
 *
 * A being is held in two separate measures. Prestige is respect earned by
 * skill, generosity and deeds; dominance is power held through force and fear.
 * Both are SosQ16 in [0, ONE] and both decay toward a small baseline that
 * every being has, so nobody has to be touched as time passes: standing is
 * computed lazily from what a record holds and the time since it was written,
 * exactly like plant regrowth. Prestige lasts about a generation, domination
 * only a few years.
 *
 * Only change is stored: one compact record per being that has ever earned or
 * lost standing, in a sparse table keyed by serial, holding the two values and
 * the tick they were last written at. A being with no record stands at the
 * baseline; a record that has decayed back to the baseline is dropped. The
 * total state therefore folds into one order-independent hash, and standing
 * follows from the seed and the sequence of awards, so a replay is comparable
 * across machines. */

/* Standing everyone is born with and rests at. About 3% of the scale, enough
 * that a person nobody knows still outranks nobody at all. */
#define SOS_STATUS_BASELINE ((SosQ16)(SOS_Q16_ONE / 32))
/* Time for half of the standing above the baseline to fade with no renewal.
 * Respect is remembered about a generation; fear fades in a few years. */
#define SOS_STATUS_PRESTIGE_HALF_LIFE (UINT64_C(20) * (uint64_t)SOS_TICKS_PER_YEAR)
#define SOS_STATUS_DOMINANCE_HALF_LIFE (UINT64_C(4) * (uint64_t)SOS_TICKS_PER_YEAR)
/* Ranking weighs respect above fear: a point of dominance counts as half a
 * point of prestige. */
#define SOS_STATUS_RANK_PRESTIGE_WEIGHT ((SosQ16)SOS_Q16_ONE)
#define SOS_STATUS_RANK_DOMINANCE_WEIGHT ((SosQ16)(SOS_Q16_ONE / 2))
/* A child begins with this share of its parents' average prestige. */
#define SOS_STATUS_INHERIT_SHARE ((SosQ16)(SOS_Q16_ONE / 4))

/* The two measures a being is held in, after lazy decay. */
typedef struct SosStanding
{
    SosQ16 prestige;
    SosQ16 dominance;
} SosStanding;

typedef struct SosStatus SosStatus;

/* An empty status, where every being stands at the baseline, or NULL when
 * memory ran out. The seed is folded into the hash, so worlds compare by seed
 * as well as by standing. */
SosStatus *SosStatusCreate(uint64_t worldSeed);
void SosStatusDestroy(SosStatus *status);

/* The standing of `being` at `tick` after lazy decay, written to
 * `outStanding`. False when `status` or `outStanding` is NULL or `being` is 0,
 * leaving `outStanding` untouched. A being with no record stands at
 * SOS_STATUS_BASELINE, and the call still succeeds. */
bool SosStatusGet(const SosStatus *status, uint64_t being, uint64_t tick, SosStanding *outStanding);

/* Adds `prestigeDelta` and `dominanceDelta` to `being` at `tick`. A positive
 * gain is lifted along a curve of diminishing returns near the top, so a long
 * career can never pass SOS_Q16_ONE; a negative delta shames at full force.
 * Nothing is stored for two zero deltas. False when `status` is NULL, `being`
 * is 0, or memory ran out, in which case the status is unchanged. */
bool SosStatusAward(SosStatus *status, uint64_t being, uint64_t tick, SosQ16 prestigeDelta,
                    SosQ16 dominanceDelta);

/* Orders `beings` by weighted standing, the highest first, into `outOrdered`,
 * which must hold `count` serials. Equal standing goes to the lower serial, so
 * the order is total and the same on every machine. Returns how many were
 * ordered (always `count` on success), or 0 when `status` or `outOrdered` is
 * NULL, `beings` is NULL with a nonzero count, or the count is 0. */
uint32_t SosStatusRank(const SosStatus *status, uint64_t tick, const uint64_t *beings,
                       uint32_t count, uint64_t *outOrdered);

/* Gives `child` SOS_STATUS_INHERIT_SHARE of its parents' average prestige at
 * `tick`, on top of the standing it already has; dominance is not inherited.
 * A parent that is 0 is left out of the average. False when `status` is NULL,
 * `child` is 0, both parents are, or memory ran out, in which case the status
 * is unchanged. */
bool SosStatusInherit(SosStatus *status, uint64_t child, uint64_t mother, uint64_t father,
                      uint64_t tick);

/* Drops any record of `being`, leaving it at the baseline; true when there was
 * one. False when `status` is NULL or `being` is 0. */
bool SosStatusForget(SosStatus *status, uint64_t being);

/* Drops every record whose standing has decayed back to the baseline at `tick`
 * and returns how many. Such a record changes nothing, so pruning never
 * changes the hash. Zero for a NULL status. */
uint32_t SosStatusPrune(SosStatus *status, uint64_t tick);

/* Folds the standing at `tick` into one value, order-independently: records
 * that have decayed to the baseline count as if they were never there, so a
 * pruned status and an unpruned one hash alike. Zero for a NULL status. */
uint64_t SosStatusHash(const SosStatus *status, uint64_t tick);

#endif
