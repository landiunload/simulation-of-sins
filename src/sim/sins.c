#include "sim/sins.h"

#include "sim/random.h"

#include <stddef.h>

/* Salt that keeps a being's sin stream apart from the streams other modules
 * open for it; 0x73696e73 spells "sins". */
#define SOS_SINS_STREAM_SALT UINT64_C(0x73696e73)

/* How a situation drives one sin, in Q16. opportunity is the occasion itself
 * and need, provocation and a rival's advantage are what it holds, while
 * `shame` and `contentment` subtract, because being watched and a good mood
 * hold a being back from every sin. SosSinsTemptation scales this whole offer
 * by the situation's opportunity, so no sin happens without an occasion, and
 * lets wrath's provocation bypass that gate. Provocation drives wrath hardest,
 * a rival's advantage drives envy, and an unmet need drives lust, gluttony and
 * sloth. */
typedef struct SinTemperament
{
    SosQ16 opportunity;
    SosQ16 need;
    SosQ16 provocation;
    SosQ16 rivalAdvantage;
    SosQ16 shame;
    SosQ16 contentment;
} SinTemperament;

static const SinTemperament temperament[SOS_SIN_COUNT] = {
    [SOS_SIN_PRIDE] = {.opportunity = SOS_Q16_ONE,
                       .need = SOS_Q16_ONE / 4,
                       .provocation = SOS_Q16_ONE / 4,
                       .rivalAdvantage = SOS_Q16_ONE / 2,
                       .shame = (3 * SOS_Q16_ONE) / 4,
                       .contentment = SOS_Q16_ONE / 4},
    [SOS_SIN_GREED] = {.opportunity = SOS_Q16_ONE,
                       .need = SOS_Q16_ONE / 2,
                       .provocation = SOS_Q16_ONE / 4,
                       .rivalAdvantage = SOS_Q16_ONE / 2,
                       .shame = (3 * SOS_Q16_ONE) / 4,
                       .contentment = SOS_Q16_ONE / 4},
    [SOS_SIN_LUST] = {.opportunity = SOS_Q16_ONE,
                      .need = (3 * SOS_Q16_ONE) / 4,
                      .provocation = SOS_Q16_ONE / 4,
                      .rivalAdvantage = SOS_Q16_ONE / 4,
                      .shame = (3 * SOS_Q16_ONE) / 4,
                      .contentment = SOS_Q16_ONE / 4},
    [SOS_SIN_ENVY] = {.opportunity = SOS_Q16_ONE / 2,
                      .need = SOS_Q16_ONE / 4,
                      .provocation = SOS_Q16_ONE / 2,
                      .rivalAdvantage = SOS_Q16_ONE,
                      .shame = (3 * SOS_Q16_ONE) / 4,
                      .contentment = SOS_Q16_ONE / 4},
    [SOS_SIN_GLUTTONY] = {.opportunity = SOS_Q16_ONE,
                          .need = (3 * SOS_Q16_ONE) / 4,
                          .provocation = SOS_Q16_ONE / 4,
                          .rivalAdvantage = SOS_Q16_ONE / 4,
                          .shame = (3 * SOS_Q16_ONE) / 4,
                          .contentment = SOS_Q16_ONE / 4},
    [SOS_SIN_WRATH] = {.opportunity = SOS_Q16_ONE / 2,
                       .need = SOS_Q16_ONE / 4,
                       .provocation = SOS_Q16_ONE,
                       .rivalAdvantage = SOS_Q16_ONE / 2,
                       .shame = (3 * SOS_Q16_ONE) / 4,
                       .contentment = SOS_Q16_ONE / 4},
    [SOS_SIN_SLOTH] = {.opportunity = SOS_Q16_ONE / 2,
                       .need = (3 * SOS_Q16_ONE) / 4,
                       .provocation = SOS_Q16_ONE / 4,
                       .rivalAdvantage = SOS_Q16_ONE / 4,
                       .shame = (3 * SOS_Q16_ONE) / 4,
                       .contentment = SOS_Q16_ONE / 2},
};

static const char *const sinNames[SOS_SIN_COUNT] = {
    [SOS_SIN_PRIDE] = "pride", [SOS_SIN_GREED] = "greed",       [SOS_SIN_LUST] = "lust",
    [SOS_SIN_ENVY] = "envy",   [SOS_SIN_GLUTTONY] = "gluttony", [SOS_SIN_WRATH] = "wrath",
    [SOS_SIN_SLOTH] = "sloth",
};

static const char *const virtueNames[SOS_SIN_COUNT] = {
    [SOS_SIN_PRIDE] = "humility",  [SOS_SIN_GREED] = "charity",       [SOS_SIN_LUST] = "chastity",
    [SOS_SIN_ENVY] = "kindness",   [SOS_SIN_GLUTTONY] = "temperance", [SOS_SIN_WRATH] = "patience",
    [SOS_SIN_SLOTH] = "diligence",
};

static bool IsSin(SosSin sin)
{
    return (unsigned)sin < (unsigned)SOS_SIN_COUNT;
}

static void IncrementCount(uint32_t *count)
{
    if (*count < UINT32_MAX)
    {
        *count += 1u;
    }
}

static SosQ16 CurrentGuilt(const SosSinRecord *record, uint64_t tick)
{
    return (SosQ16)SosDecaySince(record->guilt, record->stamp, tick, SOS_SINS_GUILT_HALF_LIFE);
}

static SosQ16 CurrentNotoriety(const SosSinRecord *record, uint64_t tick)
{
    return (SosQ16)SosDecaySince(record->notoriety, record->stamp, tick,
                                 SOS_SINS_NOTORIETY_HALF_LIFE);
}

/* Moves the stored guilt and notoriety up to `tick` so that a change can be
 * added to what is really there; the time since the last write is charged once
 * per half-life passed, however it is split. */
static void Materialize(SosSinRecord *record, uint64_t tick)
{
    const SosQ16 guilt = CurrentGuilt(record, tick);
    const SosQ16 notoriety = CurrentNotoriety(record, tick);
    record->guilt = guilt;
    record->notoriety = notoriety;
    record->stamp = tick;
}

bool SosSinsNature(uint64_t seed, uint64_t serial, const SosQ16 predisposition[SOS_SIN_COUNT],
                   SosSinNature *out)
{
    if (predisposition == NULL || out == NULL)
    {
        return false;
    }
    SosRandom random = SosRandomStream(seed, SosHashCombine(serial, SOS_SINS_STREAM_SALT));
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        const SosQ16 base = SosQ16Clamp(predisposition[sin], 0, SOS_Q16_ONE);
        const int64_t variation = SosRandomRange(&random, -SOS_SINS_VARIATION, SOS_SINS_VARIATION);
        out->tendency[sin] = SosQ16Clamp((SosQ16)((int64_t)base + variation), 0, SOS_Q16_ONE);
    }
    return true;
}

SosQ16 SosSinsTemptation(const SosSinNature *nature, SosSin sin, const SosSinSituation *situation)
{
    if (nature == NULL || situation == NULL || !IsSin(sin))
    {
        return 0;
    }
    const SinTemperament *temper = &temperament[sin];
    const SosQ16 provocation =
        SosQ16Mul(temper->provocation, SosQ16Clamp(situation->provocation, 0, SOS_Q16_ONE));
    int64_t offer = temper->opportunity;
    offer += SosQ16Mul(temper->need, SosQ16Clamp(situation->need, 0, SOS_Q16_ONE));
    offer += provocation;
    offer +=
        SosQ16Mul(temper->rivalAdvantage, SosQ16Clamp(situation->rivalAdvantage, 0, SOS_Q16_ONE));
    offer -= SosQ16Mul(temper->shame, SosQ16Clamp(situation->watched, 0, SOS_Q16_ONE));
    offer -=
        SosQ16Mul(temper->contentment, SosQ16Clamp(situation->mood, -SOS_Q16_ONE, SOS_Q16_ONE));
    /* The opportunity is the occasion: it gates the whole offer, so a
     * situation that offers none tempts nobody. */
    int64_t pressure = SosQ16Mul(SosQ16Clamp((SosQ16)offer, 0, SOS_Q16_ONE),
                                 SosQ16Clamp(situation->opportunity, 0, SOS_Q16_ONE));
    if (sin == SOS_SIN_WRATH)
    {
        /* An injury already struck is its own occasion, so wrath answers
         * provocation where there is no opportunity at all. */
        pressure += provocation;
    }
    return SosQ16Mul(SosQ16Clamp(nature->tendency[sin], 0, SOS_Q16_ONE),
                     SosQ16Clamp((SosQ16)pressure, 0, SOS_Q16_ONE));
}

bool SosSinsCommit(SosSinNature *nature, SosSinRecord *record, SosSin sin, SosQ16 severity,
                   uint64_t tick, bool witnessed)
{
    if (nature == NULL || record == NULL || !IsSin(sin))
    {
        return false;
    }
    const SosQ16 amount = SosQ16Clamp(severity, 0, SOS_Q16_ONE);
    Materialize(record, tick);
    IncrementCount(&record->committed[sin]);
    nature->tendency[sin] =
        SosQ16AddClamped(SosQ16Clamp(nature->tendency[sin], 0, SOS_Q16_ONE),
                         SosQ16Mul(amount, SOS_SINS_HABIT_GAIN), 0, SOS_Q16_ONE);
    record->guilt =
        SosQ16AddClamped(record->guilt, SosQ16Mul(amount, SOS_SINS_GUILT_PER_SIN), 0, SOS_Q16_ONE);
    if (witnessed)
    {
        record->notoriety = SosQ16AddClamped(
            record->notoriety, SosQ16Mul(amount, SOS_SINS_NOTORIETY_PER_SIN), 0, SOS_Q16_ONE);
    }
    return true;
}

bool SosSinsResist(SosSinNature *nature, SosSinRecord *record, SosSin sin, uint64_t tick)
{
    if (nature == NULL || record == NULL || !IsSin(sin))
    {
        return false;
    }
    Materialize(record, tick);
    IncrementCount(&record->resisted[sin]);
    nature->tendency[sin] = SosQ16AddClamped(SosQ16Clamp(nature->tendency[sin], 0, SOS_Q16_ONE),
                                             (SosQ16)-SOS_SINS_HABIT_LOSS, 0, SOS_Q16_ONE);
    record->guilt =
        SosQ16AddClamped(record->guilt, (SosQ16)-SOS_SINS_RESIST_RELIEF, 0, SOS_Q16_ONE);
    return true;
}

SosQ16 SosSinsGuilt(const SosSinRecord *record, uint64_t tick)
{
    return record == NULL ? 0 : CurrentGuilt(record, tick);
}

SosQ16 SosSinsNotoriety(const SosSinRecord *record, uint64_t tick)
{
    return record == NULL ? 0 : CurrentNotoriety(record, tick);
}

SosQ16 SosSinsBalance(const SosSinRecord *record, uint64_t tick)
{
    if (record == NULL)
    {
        return 0;
    }
    int64_t resisted = 0;
    int64_t committed = 0;
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        resisted += record->resisted[sin];
        committed += record->committed[sin];
    }
    /* One is added to the denominator so a single act bends the balance
     * toward virtue without pinning it, and so an empty ledger divides by
     * something. */
    const SosQ16 conduct = SosQ16Ratio(resisted - committed, resisted + committed + 1);
    const int64_t balance = (int64_t)conduct -
                            SosQ16Mul(CurrentGuilt(record, tick), SOS_SINS_GUILT_WEIGHT) -
                            SosQ16Mul(CurrentNotoriety(record, tick), SOS_SINS_NOTORIETY_WEIGHT);
    if (balance <= -SOS_Q16_ONE)
    {
        return -SOS_Q16_ONE;
    }
    return balance >= SOS_Q16_ONE ? SOS_Q16_ONE : (SosQ16)balance;
}

uint64_t SosSinsHash(const SosSinRecord *record, uint64_t tick)
{
    if (record == NULL)
    {
        return 0u;
    }
    /* A sum does not depend on the order the sins are visited in. */
    uint64_t sum = 0u;
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        uint64_t value = SosHashCombine(record->committed[sin], record->resisted[sin]);
        value = SosHashCombine(value, sin);
        sum += SosHash64(value);
    }
    sum += SosHash64((uint64_t)CurrentGuilt(record, tick));
    sum += SosHash64((uint64_t)CurrentNotoriety(record, tick));
    /* The stamp moves on every write, and two ledgers that look decayed alike
     * are still different states if they were written at different moments. */
    sum += SosHash64(record->stamp);
    return sum;
}

uint64_t SosSinsNatureHash(const SosSinNature *nature)
{
    if (nature == NULL)
    {
        return 0u;
    }
    /* A sum does not depend on the order the sins are visited in. */
    uint64_t sum = 0u;
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        const uint64_t value = SosHashCombine((uint64_t)(uint32_t)nature->tendency[sin], sin);
        sum += SosHash64(value);
    }
    return sum;
}

const char *SosSinsName(SosSin sin)
{
    return IsSin(sin) ? sinNames[sin] : NULL;
}

const char *SosSinsVirtueName(SosSin sin)
{
    return IsSin(sin) ? virtueNames[sin] : NULL;
}
