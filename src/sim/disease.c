#include "sim/disease.h"

#include "sim/biology.h"

#include <stddef.h>

/* The low SOS_DISEASE_DRIFT_BITS of a strain id are drift, the rest name the
 * lineage. A property is the lineage's hashed base plus a drift offset of at
 * most half the property span, so a strain and its mutants overlap heavily
 * while unrelated lineages spread over the whole range. */
#define SOS_DISEASE_DRIFT_RANGE (UINT64_C(1) << SOS_DISEASE_DRIFT_BITS)
#define SOS_DISEASE_DRIFT_MASK (SOS_DISEASE_DRIFT_RANGE - 1u)
#define SOS_DISEASE_DRIFT_HALF (SOS_DISEASE_DRIFT_RANGE / 2u)
/* One mutation moves the drift at most an eighth of its range. With the
 * half-span drift that shifts a property by at most 1/8 of its span. */
#define SOS_DISEASE_MUTATION_STEP (SOS_DISEASE_DRIFT_RANGE / 8u)

/* Stream tag separating pathogen lineages from every other world function. */
#define SOS_DISEASE_LINEAGE_STREAM UINT64_C(0x64697365617365) /* "disease" */

/* Pathogen property ranges. Transmissibility and severity are Q16 shares of a
 * full chance and of a full health bar; the tick ranges set a disease that
 * incubates for hours to days, spreads for days and is forgotten in months. */
#define SOS_DISEASE_TRANSMISSIBILITY_MIN (SOS_Q16_ONE / 10)
#define SOS_DISEASE_TRANSMISSIBILITY_MAX (9 * SOS_Q16_ONE / 10)
#define SOS_DISEASE_INCUBATION_MIN (SOS_TICKS_PER_HOUR)
#define SOS_DISEASE_INCUBATION_MAX (8 * SOS_TICKS_PER_DAY)
#define SOS_DISEASE_INFECTIOUS_MIN (SOS_TICKS_PER_DAY)
#define SOS_DISEASE_INFECTIOUS_MAX (12 * SOS_TICKS_PER_DAY)
/* The worst strain loses this share of a health bar per day, chosen so a
 * strain at the top of both ranges (12 infectious days) costs exactly a full
 * bar. Severity is drawn skewed low (see DrawSeverity) and antigenic drift
 * spreads it, which puts the mean cost of an infection near a quarter of a
 * bar: the average strain does not kill, and deaths come from the lethality
 * roll and from the rare strain that runs long and hot. */
#define SOS_DISEASE_SEVERITY_MAX (SOS_Q16_ONE / 12)
#define SOS_DISEASE_LETHALITY_MAX (SOS_Q16_ONE / 5)
#define SOS_DISEASE_IMMUNITY_MIN (30 * SOS_TICKS_PER_DAY)
#define SOS_DISEASE_IMMUNITY_MAX (2 * SOS_TICKS_PER_YEAR)

/* a + b, stopping at the largest tick instead of wrapping. */
static uint64_t SaturatingAdd(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

/* a - b, stopping at zero instead of wrapping. */
static uint64_t SaturatingSub(uint64_t a, uint64_t b)
{
    return a < b ? 0u : a - b;
}

/* Whether `duration` ticks have passed since `start` at `tick`, without
 * computing `start + duration` and so without wrapping. */
static bool Elapsed(uint64_t tick, uint64_t start, uint64_t duration)
{
    return tick >= start && tick - start >= duration;
}

static SosQ16 DrawFraction(SosRandom *random, SosQ16 minimum, SosQ16 maximum)
{
    return SosQ16Lerp(minimum, maximum, (SosQ16)SosRandomBelow(random, (uint32_t)SOS_Q16_ONE));
}

static uint64_t DrawTicks(SosRandom *random, int64_t minimum, int64_t maximum)
{
    return (uint64_t)SosRandomRange(random, minimum, maximum);
}

/* Severity, skewed toward zero: squaring a uniform fraction keeps most strains
 * mild with a long tail, so the heavy hitters stay rare. One draw, like the
 * other properties, to keep the stream stable. */
static SosQ16 DrawSeverity(SosRandom *random)
{
    const SosQ16 uniform = DrawFraction(random, 0, SOS_Q16_ONE);
    return SosQ16Mul(SOS_DISEASE_SEVERITY_MAX, SosQ16Mul(uniform, uniform));
}

/* `base` shifted by the drift `centered` in [-HALF, HALF): at most half the
 * span, and identically zero for a lineage with no drift. */
static SosQ16 DriftFraction(SosQ16 base, int64_t centered, SosQ16 minimum, SosQ16 maximum)
{
    const int64_t span = (int64_t)maximum - (int64_t)minimum;
    const int64_t value = (int64_t)base + centered * span / (int64_t)SOS_DISEASE_DRIFT_RANGE;
    if (value < (int64_t)minimum)
    {
        return minimum;
    }
    return value > (int64_t)maximum ? maximum : (SosQ16)value;
}

static uint64_t DriftTicks(uint64_t base, int64_t centered, uint64_t minimum, uint64_t maximum)
{
    const int64_t span = (int64_t)(maximum - minimum);
    const int64_t value = (int64_t)base + centered * span / (int64_t)SOS_DISEASE_DRIFT_RANGE;
    if (value < (int64_t)minimum)
    {
        return minimum;
    }
    return value > (int64_t)maximum ? maximum : (uint64_t)value;
}

bool SosDiseasePathogen(uint64_t seed, uint64_t id, SosPathogen *out)
{
    if (out == NULL || id == 0u)
    {
        return false;
    }
    const uint64_t lineage = id >> SOS_DISEASE_DRIFT_BITS;
    const int64_t centered =
        (int64_t)(id & SOS_DISEASE_DRIFT_MASK) - (int64_t)SOS_DISEASE_DRIFT_HALF;
    SosRandom random = SosRandomStream(seed, SosHashCombine(lineage, SOS_DISEASE_LINEAGE_STREAM));
    const SosQ16 transmissibility =
        DrawFraction(&random, SOS_DISEASE_TRANSMISSIBILITY_MIN, SOS_DISEASE_TRANSMISSIBILITY_MAX);
    const uint64_t incubation =
        DrawTicks(&random, SOS_DISEASE_INCUBATION_MIN, SOS_DISEASE_INCUBATION_MAX);
    const uint64_t infectious =
        DrawTicks(&random, SOS_DISEASE_INFECTIOUS_MIN, SOS_DISEASE_INFECTIOUS_MAX);
    const SosQ16 severity = DrawSeverity(&random);
    const SosQ16 lethality = DrawFraction(&random, 0, SOS_DISEASE_LETHALITY_MAX);
    const uint64_t immunity =
        DrawTicks(&random, SOS_DISEASE_IMMUNITY_MIN, SOS_DISEASE_IMMUNITY_MAX);
    const SosPathogenRoute route =
        (SosPathogenRoute)SosRandomBelow(&random, (uint32_t)SOS_PATHOGEN_ROUTE_COUNT);
    *out = (SosPathogen){
        .transmissibility =
            DriftFraction(transmissibility, centered, SOS_DISEASE_TRANSMISSIBILITY_MIN,
                          SOS_DISEASE_TRANSMISSIBILITY_MAX),
        .incubationTicks = DriftTicks(incubation, centered, SOS_DISEASE_INCUBATION_MIN,
                                      SOS_DISEASE_INCUBATION_MAX),
        .infectiousTicks = DriftTicks(infectious, centered, SOS_DISEASE_INFECTIOUS_MIN,
                                      SOS_DISEASE_INFECTIOUS_MAX),
        .severity = DriftFraction(severity, centered, 0, SOS_DISEASE_SEVERITY_MAX),
        .lethality = DriftFraction(lethality, centered, 0, SOS_DISEASE_LETHALITY_MAX),
        .immunityTicks =
            DriftTicks(immunity, centered, SOS_DISEASE_IMMUNITY_MIN, SOS_DISEASE_IMMUNITY_MAX),
        .route = route,
    };
    return true;
}

bool SosDiseaseExpose(SosInfection *inf, const SosPathogen *p, uint64_t id, uint64_t tick,
                      SosQ16 closeness, uint64_t durationTicks, SosRandom *random)
{
    if (inf == NULL || p == NULL || random == NULL || id == 0u ||
        inf->stage == SOS_INFECTION_EXPOSED || inf->stage == SOS_INFECTION_INFECTIOUS ||
        (inf->stage == SOS_INFECTION_RECOVERED && tick < inf->immuneUntil))
    {
        return false;
    }
    const SosQ16 rate = SosQ16Mul(SosQ16Clamp(p->transmissibility, 0, SOS_Q16_ONE),
                                  SosQ16Clamp(closeness, 0, SOS_Q16_ONE));
    if (rate <= 0 || durationTicks == 0u)
    {
        return false;
    }
    /* A rate of one and a contact of one unit is certain; a firmer rate or a
     * longer contact stays certain rather than wrapping. */
    const uint64_t perUnit = (uint64_t)(uint32_t)rate;
    if (durationTicks <= UINT64_MAX / perUnit)
    {
        const uint64_t scaled = perUnit * durationTicks / (uint64_t)SOS_DISEASE_EXPOSURE_TICKS;
        if (scaled < (uint64_t)SOS_Q16_ONE &&
            !SosRandomChance(random, (uint32_t)scaled, (uint32_t)SOS_Q16_ONE))
        {
            return false;
        }
    }
    inf->pathogen = id;
    inf->stage = SOS_INFECTION_EXPOSED;
    inf->stageTick = tick;
    inf->immuneUntil = 0u;
    return true;
}

SosDiseaseOutcome SosDiseaseAdvance(SosInfection *inf, const SosPathogen *p, uint64_t tick)
{
    SosDiseaseOutcome outcome = {0};
    if (inf == NULL || p == NULL)
    {
        return outcome;
    }
    if (inf->stage == SOS_INFECTION_EXPOSED)
    {
        if (!Elapsed(tick, inf->stageTick, p->incubationTicks))
        {
            return outcome;
        }
        inf->stageTick = SaturatingAdd(inf->stageTick, p->incubationTicks);
        inf->stage = SOS_INFECTION_INFECTIOUS;
        outcome.becameInfectious = true;
    }
    if (inf->stage == SOS_INFECTION_INFECTIOUS)
    {
        if (!Elapsed(tick, inf->stageTick, p->infectiousTicks))
        {
            return outcome;
        }
        inf->stageTick = SaturatingAdd(inf->stageTick, p->infectiousTicks);
        inf->stage = SOS_INFECTION_RECOVERED;
        inf->immuneUntil = SaturatingAdd(inf->stageTick, p->immunityTicks);
        outcome.recovered = true;
    }
    if (inf->stage == SOS_INFECTION_RECOVERED && tick >= inf->immuneUntil)
    {
        inf->stage = SOS_INFECTION_SUSCEPTIBLE;
        inf->pathogen = 0u;
        /* Name the tick immunity ended rather than the call tick, so coarse
         * and fine advances land on the same value. */
        inf->stageTick = inf->immuneUntil;
        outcome.cleared = true;
    }
    return outcome;
}

uint32_t SosDiseaseDamage(const SosInfection *inf, const SosPathogen *p, uint64_t fromTick,
                          uint64_t ticks)
{
    if (inf == NULL || p == NULL || ticks == 0u || p->severity <= 0)
    {
        return 0u;
    }
    uint64_t start = 0u;
    uint64_t end = 0u;
    switch (inf->stage)
    {
    case SOS_INFECTION_EXPOSED:
        start = SaturatingAdd(inf->stageTick, p->incubationTicks);
        end = SaturatingAdd(start, p->infectiousTicks);
        break;
    case SOS_INFECTION_INFECTIOUS:
        start = inf->stageTick;
        end = SaturatingAdd(inf->stageTick, p->infectiousTicks);
        break;
    case SOS_INFECTION_RECOVERED:
        end = inf->stageTick;
        start = SaturatingSub(inf->stageTick, p->infectiousTicks);
        break;
    case SOS_INFECTION_SUSCEPTIBLE:
    default:
        return 0u;
    }
    const uint64_t until = SaturatingAdd(fromTick, ticks);
    const uint64_t first = fromTick > start ? fromTick : start;
    const uint64_t last = until < end ? until : end;
    if (last <= first)
    {
        return 0u;
    }
    const uint64_t overlap = last - first;
    /* Health per day is a Q16 share of the being scale. */
    const uint64_t perDay =
        (uint64_t)(uint32_t)p->severity * (uint64_t)SOS_NEED_MAX / (uint64_t)SOS_Q16_ONE;
    const uint64_t days = overlap / (uint64_t)SOS_TICKS_PER_DAY;
    const uint64_t remainder = overlap % (uint64_t)SOS_TICKS_PER_DAY;
    if (days > (uint64_t)SOS_NEED_MAX / perDay)
    {
        return (uint32_t)SOS_NEED_MAX;
    }
    /* Split the fraction so no intermediate product wraps. */
    const uint64_t parts =
        (perDay / (uint64_t)SOS_TICKS_PER_DAY) * remainder +
        (perDay % (uint64_t)SOS_TICKS_PER_DAY) * remainder / (uint64_t)SOS_TICKS_PER_DAY;
    const uint64_t damage = days * perDay + parts;
    return damage > (uint64_t)SOS_NEED_MAX ? (uint32_t)SOS_NEED_MAX : (uint32_t)damage;
}

bool SosDiseaseContagious(const SosInfection *inf, uint64_t tick)
{
    return inf != NULL && inf->stage == SOS_INFECTION_INFECTIOUS && tick >= inf->stageTick;
}

uint64_t SosDiseaseMutate(uint64_t seed, uint64_t id, SosRandom *random)
{
    if (random == NULL || id == 0u)
    {
        return 0u;
    }
    const uint64_t lineage = id >> SOS_DISEASE_DRIFT_BITS;
    const uint64_t drift = id & SOS_DISEASE_DRIFT_MASK;
    const uint64_t step =
        1u + (uint64_t)SosRandomBelow(random, (uint32_t)SOS_DISEASE_MUTATION_STEP);
    const bool up = ((SosRandomNext(random) ^ SosHashCombine(seed, id)) & 1u) != 0u;
    uint64_t moved = up ? drift + step : (drift >= step ? drift - step : drift + step);
    if (moved >= SOS_DISEASE_DRIFT_RANGE)
    {
        /* Stepping up off the top turns around; step is far below the range. */
        moved = drift - step;
    }
    return (lineage << SOS_DISEASE_DRIFT_BITS) | moved;
}
