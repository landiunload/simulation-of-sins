#include "sim/census.h"

#include "sim/calendar.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

/* The four needs share SOS_NEED_MAX as their full scale. */
_Static_assert(SOS_CENSUS_SEX_COUNT == (uint32_t)SOS_SEX_MALE + 1u,
               "the sex count must match SosSex");
_Static_assert(SOS_CENSUS_ACTIVITY_COUNT == (uint32_t)SOS_ACTIVITY_FORAGE + 1u,
               "the activity count must match SosActivity");

enum
{
    MEASURE_HEALTH = 0,
    MEASURE_HUNGER,
    MEASURE_THIRST,
    MEASURE_FATIGUE,
    MEASURE_COUNT
};

/* Age in ticks: never negative, so a birth ahead of the tick counts as zero
 * rather than wrapping. */
static uint64_t AgeTicks(int64_t birthTick, uint64_t tick)
{
    if (tick <= (uint64_t)INT64_MAX && birthTick > (int64_t)tick)
    {
        return 0u;
    }
    return tick - (uint64_t)birthTick;
}

static uint64_t AgeYears(int64_t birthTick, uint64_t tick)
{
    return AgeTicks(birthTick, tick) / (uint64_t)SOS_TICKS_PER_YEAR;
}

/* The five-year bucket of an age; everything from a hundred years on shares
 * the open-ended top bucket. */
static uint32_t AgeBucket(uint64_t years)
{
    const uint64_t bucket = years / (uint64_t)SOS_CENSUS_AGE_BUCKET_YEARS;
    return bucket < (uint64_t)SOS_CENSUS_AGE_BUCKETS - 1u ? (uint32_t)bucket
                                                          : SOS_CENSUS_AGE_BUCKETS - 1u;
}

static void AddValue(uint64_t *sum, uint32_t *minimum, uint32_t value)
{
    *sum += value;
    if (value < *minimum)
    {
        *minimum = value;
    }
}

static SosCensusMeasure Measure(uint64_t sum, uint32_t minimum, uint32_t count)
{
    if (count == 0u)
    {
        return (SosCensusMeasure){.mean = 0, .minimum = 0u};
    }
    /* The average raw value as a share of the full need scale. The count times
     * SOS_NEED_MAX fits 64 bits for any living count. */
    return (SosCensusMeasure){
        .mean = SosQ16Ratio((int64_t)sum, (int64_t)count * (int64_t)SOS_NEED_MAX),
        .minimum = minimum,
    };
}

/* The lower median age in years, found from the pyramid and, when the median
 * bucket is a closed one, refined to the year with a second pass. */
static uint64_t MedianYear(const SosBeings *beings, uint64_t tick,
                           const uint32_t pyramid[SOS_CENSUS_SEX_COUNT][SOS_CENSUS_AGE_BUCKETS],
                           uint32_t total)
{
    if (total == 0u)
    {
        return 0u;
    }
    const uint32_t rank = (total - 1u) / 2u;
    uint32_t before = 0u;
    uint32_t medianBucket = SOS_CENSUS_AGE_BUCKETS - 1u;
    for (uint32_t bucket = 0u; bucket < SOS_CENSUS_AGE_BUCKETS; ++bucket)
    {
        const uint32_t count = pyramid[SOS_SEX_FEMALE][bucket] + pyramid[SOS_SEX_MALE][bucket];
        if (before + count > rank)
        {
            medianBucket = bucket;
            break;
        }
        before += count;
    }
    /* The open-ended top bucket names no single year; report its lower edge. */
    if (medianBucket >= SOS_CENSUS_AGE_BUCKETS - 1u)
    {
        return (uint64_t)(SOS_CENSUS_AGE_BUCKETS - 1u) * SOS_CENSUS_AGE_BUCKET_YEARS;
    }
    const uint32_t wanted = rank - before;
    uint32_t within[SOS_CENSUS_AGE_BUCKET_YEARS] = {0u, 0u, 0u, 0u, 0u};
    const uint32_t slots = SosBeingsSlotCount(beings);
    for (uint32_t slot = 0u; slot < slots; ++slot)
    {
        SosBeingView view;
        if (!SosBeingsView(beings, slot, &view))
        {
            continue;
        }
        const uint64_t years = AgeYears(view.birthTick, tick);
        if (AgeBucket(years) == medianBucket)
        {
            within[years % SOS_CENSUS_AGE_BUCKET_YEARS] += 1u;
        }
    }
    uint32_t cumulative = 0u;
    for (uint32_t year = 0u; year < SOS_CENSUS_AGE_BUCKET_YEARS; ++year)
    {
        if (cumulative + within[year] > wanted)
        {
            return (uint64_t)medianBucket * SOS_CENSUS_AGE_BUCKET_YEARS + year;
        }
        cumulative += within[year];
    }
    return (uint64_t)medianBucket * SOS_CENSUS_AGE_BUCKET_YEARS +
           (SOS_CENSUS_AGE_BUCKET_YEARS - 1u);
}

bool SosCensusTake(const SosBeings *beings, uint64_t tick, SosCensus *out)
{
    if (beings == NULL || out == NULL)
    {
        return false;
    }
    SosCensus census = {.tick = tick};
    uint64_t sums[MEASURE_COUNT] = {0u, 0u, 0u, 0u};
    uint32_t minima[MEASURE_COUNT] = {SOS_NEED_MAX, SOS_NEED_MAX, SOS_NEED_MAX, SOS_NEED_MAX};
    uint64_t eldestAge = 0u;
    bool foundEldest = false;
    const uint32_t slots = SosBeingsSlotCount(beings);
    for (uint32_t slot = 0u; slot < slots; ++slot)
    {
        SosBeingView view;
        if (!SosBeingsView(beings, slot, &view))
        {
            continue;
        }
        const uint64_t ageTicks = AgeTicks(view.birthTick, tick);
        const uint32_t sex = (uint32_t)view.sex;
        const uint32_t bucket = AgeBucket(ageTicks / (uint64_t)SOS_TICKS_PER_YEAR);
        census.total += 1u;
        census.bySex[sex] += 1u;
        census.age[sex][bucket] += 1u;
        const uint32_t activity = (uint32_t)view.activity;
        if (activity < SOS_CENSUS_ACTIVITY_COUNT)
        {
            census.byActivity[activity] += 1u;
        }
        if (view.pregnant)
        {
            census.pregnant += 1u;
        }
        AddValue(&sums[MEASURE_HEALTH], &minima[MEASURE_HEALTH], view.health);
        AddValue(&sums[MEASURE_HUNGER], &minima[MEASURE_HUNGER], view.hunger);
        AddValue(&sums[MEASURE_THIRST], &minima[MEASURE_THIRST], view.thirst);
        AddValue(&sums[MEASURE_FATIGUE], &minima[MEASURE_FATIGUE], view.fatigue);
        if (!foundEldest || ageTicks > eldestAge ||
            (ageTicks == eldestAge && view.serial > census.eldestSerial))
        {
            eldestAge = ageTicks;
            census.eldestSerial = view.serial;
            foundEldest = true;
        }
    }
    census.health = Measure(sums[MEASURE_HEALTH], minima[MEASURE_HEALTH], census.total);
    census.hunger = Measure(sums[MEASURE_HUNGER], minima[MEASURE_HUNGER], census.total);
    census.thirst = Measure(sums[MEASURE_THIRST], minima[MEASURE_THIRST], census.total);
    census.fatigue = Measure(sums[MEASURE_FATIGUE], minima[MEASURE_FATIGUE], census.total);

    uint32_t young = 0u;
    uint32_t old = 0u;
    for (uint32_t bucket = 0u; bucket < SOS_CENSUS_AGE_BUCKETS; ++bucket)
    {
        const uint32_t count =
            census.age[SOS_SEX_FEMALE][bucket] + census.age[SOS_SEX_MALE][bucket];
        const uint32_t firstYear = bucket * SOS_CENSUS_AGE_BUCKET_YEARS;
        if (firstYear + SOS_CENSUS_AGE_BUCKET_YEARS <= SOS_CENSUS_DEPENDENT_YOUNG)
        {
            young += count;
        }
        else if (firstYear >= SOS_CENSUS_DEPENDENT_OLD)
        {
            old += count;
        }
    }
    census.dependencyRatio = SosQ16Ratio((int64_t)young + (int64_t)old,
                                         (int64_t)census.total - (int64_t)young - (int64_t)old);
    census.medianAgeYears = MedianYear(beings, tick, census.age, census.total);
    *out = census;
    return true;
}

/* A Q16 result from a division that may leave the Q16 range. */
static SosQ16 ClampQ16(int64_t value)
{
    return value > INT32_MAX ? INT32_MAX : (value < INT32_MIN ? INT32_MIN : (SosQ16)value);
}

SosQ16 SosCensusGrowthPerYear(const SosCensus *before, const SosCensus *after,
                              uint64_t ticksBetween)
{
    if (before == NULL || after == NULL || before->total == 0u || ticksBetween == 0u)
    {
        return 0;
    }
    /* Both totals fit 32 bits, so the change fits a signed 64-bit value. */
    const int64_t change = (int64_t)after->total - (int64_t)before->total;
    const int64_t ticks = ticksBetween > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)ticksBetween;
    /* change / before as a Q16, times a year's ticks over the elapsed ticks
     * gives the change per year: multiplying by the rate per tick instead of
     * dividing by the years keeps a fraction of a year exact. The product of a
     * Q16 and a year's ticks fits 64 bits, and a very long interval simply
     * divides it back down to zero. */
    const SosQ16 perPopulation = SosQ16Ratio(change, (int64_t)before->total);
    return ClampQ16((int64_t)perPopulation * SOS_TICKS_PER_YEAR / ticks);
}

/* A growing text buffer that can never write past its capacity and always
 * leaves a NUL; once a line does not fit, the rest is dropped. */
typedef struct Text
{
    char *out;
    size_t capacity;
    size_t length;
    bool truncated;
} Text;

static void Append(Text *text, const char *format, ...)
{
    if (text->truncated)
    {
        return;
    }
    const size_t remaining = text->capacity - text->length;
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(text->out + text->length, remaining, format, arguments);
    va_end(arguments);
    if (written < 0)
    {
        text->truncated = true;
        return;
    }
    const size_t added = (size_t)written;
    if (added >= remaining)
    {
        text->length = text->capacity - 1u;
        text->truncated = true;
    }
    else
    {
        text->length += added;
    }
}

/* A Q16 value with two decimals, using truncation like SosQ16Ratio. */
static void AppendQ16(Text *text, SosQ16 value)
{
    const int64_t magnitude = value < 0 ? -(int64_t)value : (int64_t)value;
    if (value < 0)
    {
        Append(text, "-");
    }
    Append(text, "%" PRId64 ".%02" PRId64, magnitude / SOS_Q16_ONE,
           magnitude % SOS_Q16_ONE * 100 / SOS_Q16_ONE);
}

/* A Q16 share of a full scale as a percentage with two decimals. */
static void AppendPercent(Text *text, SosQ16 share)
{
    const int64_t scaled = (int64_t)share * 100;
    Append(text, "%" PRId64 ".%02" PRId64 "%%", scaled / SOS_Q16_ONE,
           scaled % SOS_Q16_ONE * 100 / SOS_Q16_ONE);
}

static void AppendPyramid(Text *text, const char *sex, const uint32_t *buckets)
{
    Append(text, "age %s:", sex);
    for (uint32_t bucket = 0u; bucket < SOS_CENSUS_AGE_BUCKETS; ++bucket)
    {
        Append(text, " %u", buckets[bucket]);
    }
    Append(text, "\n");
}

bool SosCensusFormat(const SosCensus *c, char *out, size_t capacity)
{
    if (c == NULL || out == NULL || capacity == 0u)
    {
        return false;
    }
    Text text = {.out = out, .capacity = capacity, .length = 0u, .truncated = false};
    out[0] = '\0';
    Append(&text, "census at tick %" PRIu64 "\n", c->tick);
    Append(&text, "population %u: female %u, male %u, pregnant %u\n", c->total,
           c->bySex[SOS_SEX_FEMALE], c->bySex[SOS_SEX_MALE], c->pregnant);
    AppendPyramid(&text, "female", c->age[SOS_SEX_FEMALE]);
    AppendPyramid(&text, "male", c->age[SOS_SEX_MALE]);
    Append(&text, "activity idle %u, wander %u, eat %u, drink %u, sleep %u, forage %u\n",
           c->byActivity[SOS_ACTIVITY_IDLE], c->byActivity[SOS_ACTIVITY_WANDER],
           c->byActivity[SOS_ACTIVITY_EAT], c->byActivity[SOS_ACTIVITY_DRINK],
           c->byActivity[SOS_ACTIVITY_SLEEP], c->byActivity[SOS_ACTIVITY_FORAGE]);
    Append(&text, "health ");
    AppendPercent(&text, c->health.mean);
    Append(&text, " (min %u), hunger ", c->health.minimum);
    AppendPercent(&text, c->hunger.mean);
    Append(&text, " (min %u), thirst ", c->hunger.minimum);
    AppendPercent(&text, c->thirst.mean);
    Append(&text, " (min %u), fatigue ", c->thirst.minimum);
    AppendPercent(&text, c->fatigue.mean);
    Append(&text, " (min %u)\n", c->fatigue.minimum);
    Append(&text, "median age %" PRIu64 " years, dependency ratio ", c->medianAgeYears);
    AppendQ16(&text, c->dependencyRatio);
    Append(&text, ", eldest %" PRIu64 "\n", c->eldestSerial);
    return !text.truncated;
}
