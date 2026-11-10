#include "sim/motives.h"

#include "sim/calendar.h"
#include "sim/random.h"

#include <stddef.h>

/* Ticks for a drive to grow from satisfied to unbearable at neutral
 * personality: survival drives bite within hours, social ones within days. */
#define HOURS(count) ((uint64_t)(count) * (uint64_t)SOS_TICKS_PER_HOUR)
#define DAYS(count) ((uint64_t)(count) * (uint64_t)SOS_TICKS_PER_DAY)

static const uint64_t baseRateTicks[SOS_MOTIVE_COUNT] = {
    [SOS_MOTIVE_WARMTH] = HOURS(4),       [SOS_MOTIVE_SAFETY] = HOURS(12),
    [SOS_MOTIVE_COMPANIONSHIP] = DAYS(1), [SOS_MOTIVE_INTIMACY] = DAYS(3),
    [SOS_MOTIVE_ESTEEM] = DAYS(2),        [SOS_MOTIVE_CURIOSITY] = HOURS(3),
    [SOS_MOTIVE_COMFORT] = HOURS(8),      [SOS_MOTIVE_PLAY] = HOURS(10),
    [SOS_MOTIVE_PURPOSE] = DAYS(4),
};

/* A drive's level plus the growth it has earned but not yet rounded into a
 * whole Q16 unit. `carry` is that fraction's numerator over the drive's rate,
 * so it keeps its meaning whether the drive is advanced once or every tick. */
typedef struct Growth
{
    SosQ16 level;
    uint32_t carry;
} Growth;

static SosQ16 ClampScale(SosQ16 value)
{
    if (value <= 0)
    {
        return 0;
    }
    return value >= SOS_MOTIVE_SCALE_MAX ? SOS_MOTIVE_SCALE_MAX : value;
}

/* The drives this module stores; everything else is unknown to it. */
static bool TrackedDrive(uint32_t motive)
{
    return motive > SOS_MOTIVE_NONE && motive < SOS_MOTIVE_COUNT;
}

/* Personality multiplier of a drive; NULL rates are neutral. Clamping here,
 * not at an init no longer exists, keeps an out-of-range caller honest. */
static SosQ16 ScaleOf(const SosMotiveRates *rates, uint32_t motive)
{
    return rates != NULL ? ClampScale(rates->scale[motive]) : SOS_Q16_ONE;
}

/* `level` and `carry` after `elapsed` ticks of a drive that reaches unbearable
 * after `rate` ticks when `scale` scales its speed. The fraction left over by
 * integer division is carried, so advancing a drive in one long step or in
 * many short ones reaches exactly the same level. */
static Growth Grow(uint64_t elapsed, SosQ16 scale, uint64_t rate, SosQ16 level, uint32_t carry)
{
    if (level >= SOS_Q16_ONE)
    {
        return (Growth){.level = SOS_Q16_ONE, .carry = 0u};
    }
    if (scale <= 0 || elapsed == 0u)
    {
        return (Growth){.level = level, .carry = carry};
    }
    /* The drive fills before this many ticks; `elapsed` staying below it keeps
     * elapsed * scale inside 64 bits. A zero `rate` means it fills at once. */
    const uint64_t full = rate * (uint64_t)SOS_Q16_ONE / (uint64_t)scale;
    if (elapsed >= full)
    {
        return (Growth){.level = SOS_Q16_ONE, .carry = 0u};
    }
    const uint64_t total = (uint64_t)carry + elapsed * (uint64_t)scale;
    const uint64_t grown = (uint64_t)level + total / rate;
    if (grown >= (uint64_t)SOS_Q16_ONE)
    {
        return (Growth){.level = SOS_Q16_ONE, .carry = 0u};
    }
    return (Growth){.level = (SosQ16)grown, .carry = (uint32_t)(total % rate)};
}

/* Grows every tracked drive to `now`, so the single counter can move forward
 * without losing or double-counting time for the drives nobody touched. */
static void Rebase(SosMotives *m, const SosMotiveRates *rates, uint64_t now)
{
    const uint64_t elapsed = now - m->lastTick;
    for (uint32_t motive = SOS_MOTIVE_WARMTH; motive < SOS_MOTIVE_COUNT; ++motive)
    {
        const Growth growth = Grow(elapsed, ScaleOf(rates, motive), baseRateTicks[motive],
                                   m->level[motive], m->carry[motive]);
        m->level[motive] = growth.level;
        m->carry[motive] = growth.carry;
    }
    m->lastTick = now;
}

SosMotiveRates SosMotiveRatesNeutral(void)
{
    SosMotiveRates rates = {0};
    for (uint32_t motive = SOS_MOTIVE_WARMTH; motive < SOS_MOTIVE_COUNT; ++motive)
    {
        rates.scale[motive] = SOS_Q16_ONE;
    }
    return rates;
}

void SosMotivesInit(SosMotives *m, uint64_t tick)
{
    if (m == NULL)
    {
        return;
    }
    *m = (SosMotives){.lastTick = tick};
}

/* The drive's whole lazy state at `tick`: level and carried fraction. Both
 * together, not the level alone, are what every later read or step grows from,
 * so anything that compares two beings must see both. */
static Growth GrowthAt(const SosMotives *m, const SosMotiveRates *rates, uint32_t motive,
                       uint64_t tick)
{
    const uint64_t elapsed = tick > m->lastTick ? tick - m->lastTick : 0u;
    return Grow(elapsed, ScaleOf(rates, motive), baseRateTicks[motive], m->level[motive],
                m->carry[motive]);
}

SosQ16 SosMotivesLevel(const SosMotives *m, const SosMotiveRates *rates, uint32_t motive,
                       uint64_t tick)
{
    if (m == NULL || !TrackedDrive(motive))
    {
        return 0;
    }
    return GrowthAt(m, rates, motive, tick).level;
}

bool SosMotivesSatisfy(SosMotives *m, const SosMotiveRates *rates, uint32_t motive, SosQ16 amount,
                       uint64_t tick)
{
    if (m == NULL || !TrackedDrive(motive) || amount <= 0)
    {
        return false;
    }
    /* A tick before the counter leaves the drives where they are. */
    const uint64_t now = tick > m->lastTick ? tick : m->lastTick;
    Rebase(m, rates, now);
    m->level[motive] = amount >= m->level[motive] ? 0 : (SosQ16)(m->level[motive] - amount);
    return true;
}

SosQ16 SosMotivesUrgency(const SosMotives *m, const SosMotiveRates *rates, uint32_t motive,
                         uint64_t tick)
{
    const SosQ16 level = SosMotivesLevel(m, rates, motive, tick);
    return SosQ16Mul(level, level);
}

SosMotive SosMotivesMostUrgent(const SosMotives *m, const SosMotiveRates *rates, uint64_t tick)
{
    if (m == NULL)
    {
        return SOS_MOTIVE_NONE;
    }
    SosMotive best = SOS_MOTIVE_WARMTH;
    SosQ16 bestUrgency = -1;
    for (uint32_t motive = SOS_MOTIVE_WARMTH; motive < SOS_MOTIVE_COUNT; ++motive)
    {
        const SosQ16 urgency = SosMotivesUrgency(m, rates, motive, tick);
        if (urgency > bestUrgency)
        {
            bestUrgency = urgency;
            best = (SosMotive)motive;
        }
    }
    return best;
}

int32_t SosMotivesChoose(const SosMotives *m, const SosMotiveRates *rates, uint64_t tick,
                         const SosMotiveOption *options, uint32_t count)
{
    if (m == NULL || options == NULL || count == 0u || count > (uint32_t)INT32_MAX)
    {
        return -1;
    }
    int32_t bestIndex = -1;
    int64_t bestScore = 0;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const SosMotiveOption *option = &options[index];
        int64_t score = option->cost > 0 ? -(int64_t)option->cost : 0;
        for (uint32_t slot = 0u; slot < SOS_MOTIVE_OPTION_SLOTS; ++slot)
        {
            const SosQ16 satisfaction =
                SosQ16Clamp(option->pairs[slot].satisfaction, 0, SOS_Q16_ONE);
            if (satisfaction == 0)
            {
                continue;
            }
            const SosQ16 urgency = SosMotivesUrgency(m, rates, option->pairs[slot].motive, tick);
            score += SosQ16Mul(urgency, satisfaction);
        }
        if (bestIndex < 0 || score > bestScore)
        {
            bestScore = score;
            bestIndex = (int32_t)index;
        }
    }
    return bestIndex;
}

uint64_t SosMotivesHash(const SosMotives *m, const SosMotiveRates *rates, uint64_t tick)
{
    if (m == NULL)
    {
        return 0u;
    }
    /* A sum does not depend on the order the drives are visited in. Each drive
     * folds its level and its carried fraction together, because a being that
     * keeps a different fraction of the next unit reaches a different level
     * later even while the two read the same now. */
    uint64_t sum = 0u;
    for (uint32_t motive = SOS_MOTIVE_WARMTH; motive < SOS_MOTIVE_COUNT; ++motive)
    {
        const Growth growth = GrowthAt(m, rates, motive, tick);
        const uint64_t level = (uint64_t)(uint32_t)growth.level;
        sum += SosHashCombine((uint64_t)motive, (uint64_t)growth.carry << 32 | level);
    }
    return sum;
}
