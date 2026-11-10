#include "sim/personality.h"

#include "sim/random.h"

#include <stddef.h>

/* The population average factor value, 0.5 in Q16. */
#define AVERAGE (SOS_Q16_ONE / 2)
/* Heritage and individual variation weigh the same: about 50% heritability,
 * the order twin studies find for the big five's factors. */
#define HERITAGE (SOS_Q16_ONE / 2)
/* Separates a being's personality stream from the other streams the same seed
 * and serial feed, so two modules cannot accidentally share draws. "person". */
#define STREAM_TAG (UINT64_C(0x706572736f6e))
/* Compatibility weighs agreeableness most, then a low neuroticism, then
 * openness; the three sum to ONE so the result stays a fraction. */
#define COMPAT_OPENNESS (SOS_Q16_ONE * 25 / 100)
#define COMPAT_AGREEABLENESS (SOS_Q16_ONE * 45 / 100)
#define COMPAT_NEUROTICISM (SOS_Q16_ONE - COMPAT_OPENNESS - COMPAT_AGREEABLENESS)

/* Keeps a factor inside [0, ONE] whatever arithmetic produced it. */
static SosQ16 ClampToFactor(int64_t value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > SOS_Q16_ONE)
    {
        return (SosQ16)SOS_Q16_ONE;
    }
    return (SosQ16)value;
}

void SosPersonalityFrom(const SosQ16 inherited[SOS_PERSONALITY_FACTOR_COUNT], uint64_t seed,
                        uint64_t serial, SosPersonality *out)
{
    if (out == NULL)
    {
        return;
    }
    SosRandom random = SosRandomStream(seed, SosHashCombine(serial, STREAM_TAG));
    for (uint32_t index = 0u; index < SOS_PERSONALITY_FACTOR_COUNT; ++index)
    {
        const SosQ16 predisposition =
            inherited == NULL ? (SosQ16)AVERAGE : ClampToFactor(inherited[index]);
        const int64_t genetic = (int64_t)predisposition - AVERAGE;
        /* Uniform in [0, ONE): the being's own, non-heritable share. */
        const int64_t own = (int64_t)SosRandomBelow(&random, (uint32_t)SOS_Q16_ONE) - AVERAGE;
        out->factor[index] = ClampToFactor(AVERAGE + (genetic + own) * HERITAGE / SOS_Q16_ONE);
    }
}

void SosPersonalityShift(SosPersonality *p, SosPersonalityFactor factor, SosQ16 delta)
{
    if (p == NULL || (int)factor < 0 || (int)factor >= (int)SOS_PERSONALITY_FACTOR_COUNT ||
        delta == 0)
    {
        return;
    }
    const int64_t value = p->factor[factor];
    /* Distance from the middle, which peaks at an extreme. Damping the nudge by
     * how central the factor is makes it settle rather than stick to 0 or ONE. */
    const int64_t distance = value > AVERAGE ? value - AVERAGE : AVERAGE - value;
    const int64_t damp = (int64_t)SOS_Q16_ONE - 2 * distance;
    p->factor[factor] = ClampToFactor(value + (int64_t)delta * damp / SOS_Q16_ONE);
}

/* 1 when the two values are equal, falling to 0 as they move apart. */
static SosQ16 Similarity(SosQ16 a, SosQ16 b)
{
    const int32_t gap = a - b;
    const int32_t magnitude = gap < 0 ? -gap : gap;
    return (SosQ16)(SOS_Q16_ONE - magnitude);
}

SosQ16 SosPersonalityCompatibility(const SosPersonality *a, const SosPersonality *b)
{
    if (a == NULL || b == NULL)
    {
        return 0;
    }
    const SosQ16 openness =
        Similarity(a->factor[SOS_PERSONALITY_OPENNESS], b->factor[SOS_PERSONALITY_OPENNESS]);
    const SosQ16 agreeableness = Similarity(a->factor[SOS_PERSONALITY_AGREEABLENESS],
                                            b->factor[SOS_PERSONALITY_AGREEABLENESS]);
    /* Calm is 1 with no neuroticism and 0 at the maximum average. */
    const SosQ16 calm = (SosQ16)(SOS_Q16_ONE - (a->factor[SOS_PERSONALITY_NEUROTICISM] +
                                                b->factor[SOS_PERSONALITY_NEUROTICISM]) /
                                                   2);
    return (SosQ16)(SosQ16Mul(COMPAT_OPENNESS, openness) +
                    SosQ16Mul(COMPAT_AGREEABLENESS, agreeableness) +
                    SosQ16Mul(COMPAT_NEUROTICISM, calm));
}

/* A signed Q16 in [-ONE, ONE): a factor is as likely to pull a behaviour up as
 * down, and the id alone fixes the sign, so the set of behaviours is endless. */
static SosQ16 BehaviourWeight(uint32_t behaviourId, uint32_t factor)
{
    const uint64_t hash = SosHashCombine(SosHash64(behaviourId), factor);
    return (SosQ16)((int32_t)(hash & 0xffffu) * 2 - SOS_Q16_ONE);
}

SosQ16 SosPersonalityTendency(const SosPersonality *p, uint32_t behaviourId)
{
    if (p == NULL)
    {
        return 0;
    }
    int64_t pull = 0;
    uint64_t scale = 0u;
    for (uint32_t index = 0u; index < SOS_PERSONALITY_FACTOR_COUNT; ++index)
    {
        const SosQ16 weight = BehaviourWeight(behaviourId, index);
        const int32_t deviation = (int32_t)p->factor[index] - AVERAGE;
        pull += (int64_t)weight * deviation;
        scale += (uint64_t)(weight < 0 ? -weight : weight);
    }
    if (scale == 0u)
    {
        return (SosQ16)AVERAGE;
    }
    /* Factors keep each deviation within ONE/2, so pull/scale is too and the
     * neutral average never leaves [0, ONE]. */
    return (SosQ16)(AVERAGE + pull / (int64_t)scale);
}
