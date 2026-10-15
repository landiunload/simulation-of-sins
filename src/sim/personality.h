#ifndef SOS_SIM_PERSONALITY_H
#define SOS_SIM_PERSONALITY_H

#include "sim/fixed.h"

#include <stdint.h>

/* Personality.
 *
 * The five factors of the big-five model: openness, conscientiousness,
 * extraversion, agreeableness and neuroticism. Each is a Q16 share in
 * [0, ONE] with 0.5 the population average. A personality is a small
 * plain-data record of five numbers, not a process: nothing is stored or
 * touched while time passes. Three reads turn those five numbers into endless
 * variety. A being is born with a personality built from an inherited
 * predisposition and its own seeded draw, about half of the spread being
 * heritable; life experience nudges factors over the years; other beings judge
 * how well they get on; and any behaviour id, of an unbounded set, asks the
 * five numbers how strongly it appeals. */

#define SOS_PERSONALITY_FACTOR_COUNT 5u

/* Factors are always used in this order; the ids need not be stable for
 * storage since a personality is stored as the whole array, not sparsely. */
typedef enum SosPersonalityFactor
{
    SOS_PERSONALITY_OPENNESS = 0,
    SOS_PERSONALITY_CONSCIENTIOUSNESS,
    SOS_PERSONALITY_EXTRAVERSION,
    SOS_PERSONALITY_AGREEABLENESS,
    SOS_PERSONALITY_NEUROTICISM,
} SosPersonalityFactor;

typedef struct SosPersonality
{
    SosQ16 factor[SOS_PERSONALITY_FACTOR_COUNT];
} SosPersonality;

/* Builds the personality of a newborn into `out`. Each factor blends the
 * matching `inherited` predisposition, one Q16 per factor, with an individual
 * draw from `seed` and `serial`: about half the spread comes from the
 * inheritance and half from the being itself, so kin resemble each other
 * without being copies. `inherited` may be NULL when no genome is known, which
 * means the average 0.5 for every factor; inherited values beyond [0, ONE] are
 * clamped first. Every factor lies in [0, ONE] and the same inputs always give
 * the same personality. Does nothing when `out` is NULL. */
void SosPersonalityFrom(const SosQ16 inherited[SOS_PERSONALITY_FACTOR_COUNT], uint64_t seed,
                        uint64_t serial, SosPersonality *out);

/* Nudges `factor` of `p` by `delta`, which may be negative, as life experience
 * does. The move shrinks with the factor's distance from the middle, so a
 * factor already near 0 or ONE barely moves and a nudge never carries it out of
 * [0, ONE]. Does nothing when `p` is NULL, when `factor` is not one of the five
 * or when `delta` is zero. */
void SosPersonalityShift(SosPersonality *p, SosPersonalityFactor factor, SosQ16 delta);

/* How well two personalities get on, from 0, incompatible, to ONE, kindred.
 * Symmetric in `a` and `b`: similar openness and agreeableness raise the
 * result, a high average neuroticism lowers it. Zero when `a` or `b` is NULL. */
SosQ16 SosPersonalityCompatibility(const SosPersonality *a, const SosPersonality *b);

/* How strongly a personality leans toward the behaviour named by
 * `behaviourId`. The id hashes to a signed weight on each factor, so the set of
 * behaviours is unbounded: any five factors answer every id in [0, ONE], with
 * ONE/2 the neutral average a personality of averages reports. Zero when `p` is
 * NULL. The same personality and id always give the same value, and different
 * ids give different values. */
SosQ16 SosPersonalityTendency(const SosPersonality *p, uint32_t behaviourId);

#endif
