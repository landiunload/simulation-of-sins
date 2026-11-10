#ifndef SOS_SIM_TECH_H
#define SOS_SIM_TECH_H

#include "sim/beings.h"
#include "sim/fixed.h"
#include "sim/random.h"

#include <stdbool.h>
#include <stdint.h>

/* Technology.
 *
 * A being's knowledge is a bitmask of discoveries; this layer gives each bit
 * its meaning. The catalog below is a fixed list of the discoveries of the
 * world, each with a name, the discoveries it needs, a difficulty and the era
 * it belongs to, so the same tree exists on every machine. A discovery is
 * named by its bit index, 0 .. SOS_TECH_COUNT - 1; bit 0 is speech and bit 1
 * is foraging, the roots the founders already have and that nothing needs.
 *
 * Prerequisites are a bitmask too, so the catalog is a directed acyclic
 * graph: every prerequisite has a smaller index than the discovery it
 * enables. Whether a being can reach a discovery, how likely it is over time,
 * what it can learn from another and which era it is in all follow from the
 * knowledge mask, the being's traits and this catalog, so no per-being state
 * is kept for it. The record type is used as `struct SosTechInfo` rather than
 * a typedef because in C the accessor function of the same name sits in a
 * different namespace than the struct tag. */

#define SOS_TECH_COUNT 30u
/* Knowledge is a 64-bit mask, so at most this many discoveries can exist. */
#define SOS_TECH_MAX_COUNT 64u

typedef enum SosTechEra
{
    /* The first discoveries: fire, stone tools and the hunt. */
    SOS_TECH_ERA_STONE = 0,
    /* Farming, pottery and settled life. */
    SOS_TECH_ERA_NEOLITHIC,
    /* Metal, the wheel and writing. */
    SOS_TECH_ERA_BRONZE,
    SOS_TECH_ERA_COUNT,
} SosTechEra;

/* One discovery. `prerequisites` is a knowledge mask that a being must hold
 * entirely before it can reach this; `difficulty` is a Q16 factor, never
 * below SOS_Q16_ONE, higher meaning longer to reach. */
struct SosTechInfo
{
    const char *name;
    uint64_t prerequisites;
    SosQ16 difficulty;
    SosTechEra era;
};

/* Copies the entry of discovery `id` into `*out`; false when `id` is not a
 * discovery (`id >= SOS_TECH_COUNT`) or `out` is NULL, leaving `*out`
 * untouched. */
bool SosTechInfo(uint32_t id, struct SosTechInfo *out);

/* Whether a being with `knowledge` has not got `id` yet and already holds
 * every prerequisite of it. False for an id past the catalog. */
bool SosTechCanDiscover(uint64_t knowledge, uint32_t id);

/* The mask of discoveries a being with `knowledge` could reach next: exactly
 * those it does not know whose prerequisites it all holds. Known discoveries
 * are never in the mask. */
uint64_t SosTechAvailable(uint64_t knowledge);

/* Q16 chance that a being with `knowledge` reaches `id` within
 * `exposureTicks` ticks of trying, given its `intelligence` and `curiosity`
 * (both Q16 and clamped to [0, 1]). Zero when `id` is past the catalog,
 * already known or not yet reachable, or when exposure, intelligence or
 * curiosity is not positive; one once the exposure is long enough.
 * Non-decreasing in intelligence, curiosity and exposure and non-increasing
 * in the discovery's difficulty. */
SosQ16 SosTechDiscoveryChance(uint64_t knowledge, uint32_t id, SosQ16 intelligence,
                              SosQ16 curiosity, uint64_t exposureTicks);

/* Rolls the chance above on `random`: true when the being reaches `id` now,
 * and then the caller sets its bit. False for a NULL `random` or when nothing
 * is reached. The chance alone decides how many draws are taken, so equal
 * inputs replay equally on every machine. */
bool SosTechTryDiscover(SosRandom *random, uint64_t knowledge, uint32_t id, SosQ16 intelligence,
                        SosQ16 curiosity, uint64_t exposureTicks);

/* The mask of discoveries `learner` could learn from `teacher` next: known to
 * the teacher, not yet known to the learner and with every prerequisite the
 * learner already holds. A lesson therefore never skips a prerequisite. */
uint64_t SosTechTeachable(uint64_t teacher, uint64_t learner);

/* Teaches `learner` one discovery from SosTechTeachable, picked from
 * `random`, taking hold with Q16 probability `aptitude`. Returns the learner's
 * new mask: the old one when `random` is NULL, nothing is teachable or the
 * lesson does not take. Every bit gained has all its prerequisites in the
 * result. */
uint64_t SosTechTeach(uint64_t teacher, uint64_t learner, SosRandom *random, SosQ16 aptitude);

/* The most advanced era among the discoveries in `knowledge`, STONE when it
 * holds none this catalog knows. */
SosTechEra SosTechEraOf(uint64_t knowledge);

#endif
