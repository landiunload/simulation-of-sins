#include "sim/lifecycle.h"

#include "sim/calendar.h"

#include <stddef.h>

/* Ages in years where a life stage ends. */
#define SOS_LIFECYCLE_INFANT_END_YEARS 2
#define SOS_LIFECYCLE_CHILD_END_YEARS 12
#define SOS_LIFECYCLE_ADOLESCENT_END_YEARS 16
#define SOS_LIFECYCLE_ADULT_END_YEARS 45

/* Nobody reaches this age: the hazard saturates and death is certain. It is
 * well past the point where survival rounds to zero, so it is a formality. */
#define SOS_LIFECYCLE_MAX_AGE_YEARS 120
#define SOS_LIFECYCLE_MAX_AGE_TICKS (SOS_LIFECYCLE_MAX_AGE_YEARS * SOS_TICKS_PER_YEAR)

/* Ten lunar months. */
#define SOS_LIFECYCLE_GESTATION_DAYS 266

/* The hazard a being faces whatever its age: accidents, untreated wounds and
 * disease, as a fraction of a year in Q16. */
#define SOS_LIFECYCLE_MAKEHAM_HAZARD 150 /* ~0.0023 per year */

/* The infant hump: at birth a baby faces this much yearly hazard, and it
 * halves about every half year, so the first year dominates a forager's death
 * toll while later childhood is far safer. */
#define SOS_LIFECYCLE_INFANT_HAZARD 19661 /* ~0.30 per year */
#define SOS_LIFECYCLE_INFANT_HALF_LIFE (SOS_TICKS_PER_YEAR / 2)

/* The adult or Gompertz part: at the reference age it adds this much yearly
 * hazard and it doubles every SOS_LIFECYCLE_GOMPERTZ_DOUBLING_YEARS after. */
#define SOS_LIFECYCLE_GOMPERTZ_REF_YEARS 15
#define SOS_LIFECYCLE_GOMPERTZ_DOUBLING_YEARS 8
#define SOS_LIFECYCLE_GOMPERTZ_HAZARD 95 /* ~0.00145 per year */

/* A hazard past which survival over any useful duration is zero; the yearly
 * hazard never needs to grow beyond it. */
#define SOS_LIFECYCLE_MAX_HAZARD (64 * SOS_Q16_ONE)

/* ln 2 in Q16, the bridge between a hazard rate and a half-life. */
#define SOS_LIFECYCLE_LN2_Q16 45426

/* The frailty that leaves the life table as it is; half the trait's range. */
#define SOS_LIFECYCLE_FRAILTY_NEUTRAL (SOS_Q16_ONE / 2)

#define SEGMENT_BITS 4u
#define SEGMENT_FRACTION_BITS (SOS_Q16_BITS - SEGMENT_BITS)

/* One age point of a piecewise-linear curve. */
typedef struct LifePoint
{
    int64_t ageTicks;
    SosQ16 value;
} LifePoint;

/* 2^(k/16) in Q16 for k = 0..16, rounded to nearest; the growth twin of the
 * decay table in fixed.c. Interpolating between them stays within 0.1%. */
static const uint32_t doubling[(1u << SEGMENT_BITS) + 1u] = {
    65536u, 68438u,  71468u,  74632u,  77936u,  81386u,  84990u,  88752u,  92682u,
    96785u, 101070u, 105545u, 110218u, 115098u, 120194u, 125515u, 131072u,
};

/* Women: infertile to fourteen, a peak plateau through the twenties, gone by
 * fifty. Fertility matters per month, so the exact shape is presentation. */
static const LifePoint femaleFertility[] = {
    {0, 0},
    {14 * SOS_TICKS_PER_YEAR, 0},
    {20 * SOS_TICKS_PER_YEAR, SOS_Q16_ONE},
    {30 * SOS_TICKS_PER_YEAR, SOS_Q16_ONE},
    {40 * SOS_TICKS_PER_YEAR, 26214},
    {50 * SOS_TICKS_PER_YEAR, 0},
    {SOS_LIFECYCLE_MAX_AGE_TICKS, 0},
};

/* Men: a later start and a slow decline that never quite reaches zero before
 * old age. */
static const LifePoint maleFertility[] = {
    {0, 0},
    {14 * SOS_TICKS_PER_YEAR, 0},
    {18 * SOS_TICKS_PER_YEAR, SOS_Q16_ONE},
    {35 * SOS_TICKS_PER_YEAR, 58982},
    {55 * SOS_TICKS_PER_YEAR, 42598},
    {70 * SOS_TICKS_PER_YEAR, 22938},
    {85 * SOS_TICKS_PER_YEAR, 0},
    {SOS_LIFECYCLE_MAX_AGE_TICKS, 0},
};

/* Linear between the surrounding points, flat outside them. */
static SosQ16 Interpolate(const LifePoint *points, size_t count, int64_t ageTicks)
{
    if (ageTicks <= points[0].ageTicks)
    {
        return points[0].value;
    }
    for (size_t index = 1u; index < count; ++index)
    {
        if (ageTicks < points[index].ageTicks)
        {
            const LifePoint *from = &points[index - 1u];
            const LifePoint *to = &points[index];
            const SosQ16 t = SosQ16Ratio(ageTicks - from->ageTicks, to->ageTicks - from->ageTicks);
            return SosQ16Lerp(from->value, to->value, t);
        }
    }
    return points[count - 1u].value;
}

/* 2^exponent with `exponent` a Q16 count of doublings, clamped to the Q16
 * range; negative exponents give fractions. */
static SosQ16 Exp2(SosQ16 exponent)
{
    int64_t whole = exponent / SOS_Q16_ONE;
    int64_t fraction = exponent % SOS_Q16_ONE;
    if (fraction < 0)
    {
        fraction += SOS_Q16_ONE;
        whole -= 1;
    }
    const uint32_t segment = (uint32_t)((uint64_t)fraction >> SEGMENT_FRACTION_BITS);
    const uint64_t within = (uint64_t)fraction & ((UINT64_C(1) << SEGMENT_FRACTION_BITS) - 1u);
    const uint64_t factor =
        doubling[segment] -
        ((uint64_t)(doubling[segment + 1u] - doubling[segment]) * within >> SEGMENT_FRACTION_BITS);
    if (whole >= 31)
    {
        return INT32_MAX;
    }
    if (whole <= -31)
    {
        return 0;
    }
    const int64_t scaled =
        whole >= 0 ? (int64_t)(factor << (unsigned)whole) : (int64_t)(factor >> (unsigned)-whole);
    return scaled > INT32_MAX ? INT32_MAX : (SosQ16)scaled;
}

/* The age in Q16 years; the caller has already bounded it. */
static SosQ16 AgeYearsQ16(int64_t ageTicks)
{
    const int64_t years = ageTicks / SOS_TICKS_PER_YEAR;
    const int64_t remainder = ageTicks % SOS_TICKS_PER_YEAR;
    return (SosQ16)(years * SOS_Q16_ONE + remainder * SOS_Q16_ONE / SOS_TICKS_PER_YEAR);
}

/* The multiplier a frailty puts on the hazard: neutral at the trait's middle,
 * half at robust zero, one and a half at frail one. */
static SosQ16 FrailtyScale(SosQ16 frailty)
{
    const SosQ16 clamped = SosQ16Clamp(frailty, 0, SOS_Q16_ONE);
    return (SosQ16)(SOS_LIFECYCLE_FRAILTY_NEUTRAL + clamped);
}

SosLifeStage SosLifecycleStage(int64_t ageTicks)
{
    if (ageTicks < 0)
    {
        return SOS_LIFE_UNBORN;
    }
    if (ageTicks < SOS_LIFECYCLE_INFANT_END_YEARS * SOS_TICKS_PER_YEAR)
    {
        return SOS_LIFE_INFANT;
    }
    if (ageTicks < SOS_LIFECYCLE_CHILD_END_YEARS * SOS_TICKS_PER_YEAR)
    {
        return SOS_LIFE_CHILD;
    }
    if (ageTicks < SOS_LIFECYCLE_ADOLESCENT_END_YEARS * SOS_TICKS_PER_YEAR)
    {
        return SOS_LIFE_ADOLESCENT;
    }
    if (ageTicks < SOS_LIFECYCLE_ADULT_END_YEARS * SOS_TICKS_PER_YEAR)
    {
        return SOS_LIFE_ADULT;
    }
    return SOS_LIFE_ELDER;
}

SosQ16 SosLifecycleFertility(SosSex sex, int64_t ageTicks)
{
    if (ageTicks < 0)
    {
        return 0;
    }
    if (sex == SOS_SEX_FEMALE)
    {
        return Interpolate(femaleFertility, sizeof(femaleFertility) / sizeof(femaleFertility[0]),
                           ageTicks);
    }
    if (sex == SOS_SEX_MALE)
    {
        return Interpolate(maleFertility, sizeof(maleFertility) / sizeof(maleFertility[0]),
                           ageTicks);
    }
    return 0;
}

SosQ16 SosLifecycleMortality(int64_t ageTicks, SosQ16 frailty)
{
    if (ageTicks < 0)
    {
        return 0;
    }
    if (ageTicks >= SOS_LIFECYCLE_MAX_AGE_TICKS)
    {
        return SOS_LIFECYCLE_MAX_HAZARD;
    }
    const int64_t yearsQ16 = AgeYearsQ16(ageTicks);
    const int64_t exponent = (yearsQ16 - (int64_t)SOS_LIFECYCLE_GOMPERTZ_REF_YEARS * SOS_Q16_ONE) /
                             SOS_LIFECYCLE_GOMPERTZ_DOUBLING_YEARS;
    const int64_t infant =
        (int64_t)SOS_LIFECYCLE_INFANT_HAZARD *
        SosDecay(SOS_Q16_ONE, (uint64_t)ageTicks, SOS_LIFECYCLE_INFANT_HALF_LIFE) / SOS_Q16_ONE;
    const int64_t adult =
        (int64_t)SOS_LIFECYCLE_GOMPERTZ_HAZARD * Exp2((SosQ16)exponent) / SOS_Q16_ONE;
    const int64_t hazard = ((int64_t)SOS_LIFECYCLE_MAKEHAM_HAZARD + infant + adult) *
                           FrailtyScale(frailty) / SOS_Q16_ONE;
    if (hazard > (int64_t)SOS_LIFECYCLE_MAX_HAZARD)
    {
        return SOS_LIFECYCLE_MAX_HAZARD;
    }
    return (SosQ16)hazard;
}

SosQ16 SosLifecycleSurvive(int64_t ageTicks, uint64_t durationTicks, SosQ16 frailty)
{
    if (ageTicks < 0 || durationTicks == 0u)
    {
        return SOS_Q16_ONE;
    }
    if (ageTicks >= SOS_LIFECYCLE_MAX_AGE_TICKS)
    {
        return 0;
    }
    const SosQ16 hazard = SosLifecycleMortality(ageTicks, frailty);
    if (hazard <= 0)
    {
        return SOS_Q16_ONE;
    }
    /* One halving every ln2 / hazard years: elapsed / halfLife is exactly
     * hazard * duration / (ln2 * year), so SosDecay returns 2^(-hazard t / ln2). */
    const uint64_t halfLife =
        (uint64_t)SOS_LIFECYCLE_LN2_Q16 * (uint64_t)SOS_TICKS_PER_YEAR / (uint64_t)hazard;
    if (halfLife == 0u)
    {
        return 0;
    }
    return (SosQ16)SosDecay(SOS_Q16_ONE, durationTicks, halfLife);
}

bool SosLifecycleDies(SosRandom *random, int64_t ageTicks, uint64_t durationTicks, SosQ16 frailty)
{
    if (random == NULL || ageTicks < 0 || durationTicks == 0u)
    {
        return false;
    }
    if (ageTicks >= SOS_LIFECYCLE_MAX_AGE_TICKS)
    {
        return true;
    }
    const SosQ16 survival = SosLifecycleSurvive(ageTicks, durationTicks, frailty);
    if (survival >= SOS_Q16_ONE)
    {
        return false;
    }
    if (survival <= 0)
    {
        return true;
    }
    return SosRandomBelow(random, (uint32_t)SOS_Q16_ONE) >= (uint32_t)survival;
}

int64_t SosLifecycleGestationTicks(void)
{
    return SOS_LIFECYCLE_GESTATION_DAYS * SOS_TICKS_PER_DAY;
}

int64_t SosLifecycleMaxAgeTicks(void)
{
    return SOS_LIFECYCLE_MAX_AGE_TICKS;
}
