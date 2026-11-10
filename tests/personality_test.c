#include "sim/personality.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
/* Enough draws for the means to settle within this tolerance of their target,
 * which is a few standard errors of the sample mean. */
#define SAMPLES 2000u
#define MEAN_TOLERANCE 900
#define HALF (SOS_Q16_ONE / 2)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "personality test failed: %s\n", message);
        exit(1);
    }
}

static void SetAll(SosPersonality *personality, SosQ16 value)
{
    for (uint32_t factor = 0u; factor < SOS_PERSONALITY_FACTOR_COUNT; ++factor)
    {
        personality->factor[factor] = value;
    }
}

static void ExpectInRange(const SosPersonality *personality)
{
    for (uint32_t factor = 0u; factor < SOS_PERSONALITY_FACTOR_COUNT; ++factor)
    {
        Expect(personality->factor[factor] >= 0 && personality->factor[factor] <= SOS_Q16_ONE,
               "a factor is a share in [0, ONE]");
    }
}

static void TestFrom(void)
{
    SosPersonality person;
    SosPersonality again;
    SosPersonality all;
    int64_t sums[SOS_PERSONALITY_FACTOR_COUNT] = {0};
    for (uint64_t serial = 1u; serial <= SAMPLES; ++serial)
    {
        SosPersonalityFrom(NULL, SEED, serial, &person);
        ExpectInRange(&person);
        SosPersonalityFrom(NULL, SEED, serial, &again);
        Expect(person.factor[0] == again.factor[0] && person.factor[4] == again.factor[4],
               "the same inputs build the same personality");
        for (uint32_t factor = 0u; factor < SOS_PERSONALITY_FACTOR_COUNT; ++factor)
        {
            sums[factor] += person.factor[factor];
        }
    }
    for (uint32_t factor = 0u; factor < SOS_PERSONALITY_FACTOR_COUNT; ++factor)
    {
        const int64_t mean = sums[factor] / (int64_t)SAMPLES;
        Expect(mean > HALF - MEAN_TOLERANCE && mean < HALF + MEAN_TOLERANCE,
               "a population without inheritance averages 0.5");
    }

    /* Inheriting the average is the same as inheriting nothing. */
    SetAll(&all, HALF);
    SosPersonalityFrom(all.factor, SEED, 7u, &person);
    SosPersonalityFrom(NULL, SEED, 7u, &again);
    Expect(person.factor[0] == again.factor[0] && person.factor[2] == again.factor[2],
           "an average inheritance is the same as none");

    /* Heritability: half of a high predisposition survives, so a high parent
     * sits near 0.75 on average and a low one near 0.25. */
    int64_t highSum = 0;
    int64_t lowSum = 0;
    for (uint64_t serial = 1u; serial <= SAMPLES; ++serial)
    {
        SetAll(&all, SOS_Q16_ONE);
        SosPersonalityFrom(all.factor, SEED, serial, &person);
        ExpectInRange(&person);
        highSum += person.factor[SOS_PERSONALITY_OPENNESS];
        SetAll(&all, 0);
        SosPersonalityFrom(all.factor, SEED, serial, &person);
        ExpectInRange(&person);
        lowSum += person.factor[SOS_PERSONALITY_OPENNESS];
    }
    Expect(highSum / (int64_t)SAMPLES > SOS_Q16_ONE * 3 / 4 - MEAN_TOLERANCE &&
               highSum / (int64_t)SAMPLES < SOS_Q16_ONE * 3 / 4 + MEAN_TOLERANCE,
           "a high predisposition raises the average");
    Expect(lowSum / (int64_t)SAMPLES > SOS_Q16_ONE / 4 - MEAN_TOLERANCE &&
               lowSum / (int64_t)SAMPLES < SOS_Q16_ONE / 4 + MEAN_TOLERANCE,
           "a low predisposition lowers the average");

    /* Out-of-range inheritance is clamped, never escaping [0, ONE]. */
    const SosPersonality wild = {{INT32_MAX, INT32_MIN, SOS_Q16_ONE * 2, -SOS_Q16_ONE, 0}};
    SosPersonalityFrom(wild.factor, SEED, 11u, &person);
    ExpectInRange(&person);

    /* Serial separates beings. */
    SosPersonalityFrom(NULL, SEED, 1u, &person);
    SosPersonalityFrom(NULL, SEED, 2u, &again);
    Expect(person.factor[0] != again.factor[0] || person.factor[1] != again.factor[1],
           "different serials give different people");

    SosPersonalityFrom(NULL, SEED, 3u, NULL);
}

static void TestShift(void)
{
    SosPersonality person;
    SetAll(&person, HALF);

    /* In the middle a nudge is not damped at all, so +2000 moves the factor by
     * 2000 and an opposite nudge undoes it. */
    SosPersonalityShift(&person, SOS_PERSONALITY_OPENNESS, 2000);
    Expect(person.factor[SOS_PERSONALITY_OPENNESS] == HALF + 2000,
           "a nudge from the middle is not damped");
    const SosQ16 raised = person.factor[SOS_PERSONALITY_OPENNESS];
    SosPersonalityShift(&person, SOS_PERSONALITY_OPENNESS, -2000);
    Expect(person.factor[SOS_PERSONALITY_OPENNESS] < raised &&
               person.factor[SOS_PERSONALITY_OPENNESS] > HALF,
           "an opposite nudge lowers the factor");

    /* The same nudge does less near the edge it heads for. */
    person.factor[SOS_PERSONALITY_OPENNESS] = HALF;
    SosPersonalityShift(&person, SOS_PERSONALITY_OPENNESS, 4000);
    const SosQ16 middleMove = person.factor[SOS_PERSONALITY_OPENNESS] - HALF;
    person.factor[SOS_PERSONALITY_OPENNESS] = SOS_Q16_ONE - 1000;
    SosPersonalityShift(&person, SOS_PERSONALITY_OPENNESS, 4000);
    const SosQ16 edgeMove = person.factor[SOS_PERSONALITY_OPENNESS] - (SOS_Q16_ONE - 1000);
    Expect(middleMove > edgeMove && edgeMove > 0, "a nudge fades near the extreme");

    person.factor[SOS_PERSONALITY_OPENNESS] = SOS_Q16_ONE;
    SosPersonalityShift(&person, SOS_PERSONALITY_OPENNESS, 4000);
    Expect(person.factor[SOS_PERSONALITY_OPENNESS] == SOS_Q16_ONE, "the top cannot be passed");
    person.factor[SOS_PERSONALITY_OPENNESS] = 0;
    SosPersonalityShift(&person, SOS_PERSONALITY_OPENNESS, -4000);
    Expect(person.factor[SOS_PERSONALITY_OPENNESS] == 0, "the bottom cannot be passed");

    /* Even an extreme nudge leaves the factor inside the range. */
    SetAll(&person, HALF);
    for (uint32_t factor = 0u; factor < SOS_PERSONALITY_FACTOR_COUNT; ++factor)
    {
        SosPersonalityShift(&person, (SosPersonalityFactor)factor, INT32_MAX);
        SosPersonalityShift(&person, (SosPersonalityFactor)factor, INT32_MIN);
        ExpectInRange(&person);
    }

    /* Nothing moves for a NULL, an unknown factor, or a zero nudge. */
    SetAll(&person, HALF);
    SosPersonalityShift(NULL, SOS_PERSONALITY_OPENNESS, 2000);
    SosPersonalityShift(&person, (SosPersonalityFactor)SOS_PERSONALITY_FACTOR_COUNT, 2000);
    SosPersonalityShift(&person, (SosPersonalityFactor)-1, 2000);
    SosPersonalityShift(&person, SOS_PERSONALITY_OPENNESS, 0);
    Expect(person.factor[SOS_PERSONALITY_OPENNESS] == HALF,
           "ignored shifts leave the person alone");
}

static void TestCompatibility(void)
{
    SosPersonality a;
    SosPersonality b;
    SosRandom random = SosRandomStream(SEED, 5u);
    for (int trial = 0; trial < 200; ++trial)
    {
        SosPersonalityFrom(NULL, SEED, (uint64_t)SosRandomBelow(&random, 1000u) + 1u, &a);
        SosPersonalityFrom(NULL, SEED, (uint64_t)SosRandomBelow(&random, 1000u) + 1u, &b);
        const SosQ16 forward = SosPersonalityCompatibility(&a, &b);
        const SosQ16 backward = SosPersonalityCompatibility(&b, &a);
        Expect(forward == backward, "compatibility is symmetric");
        Expect(forward >= 0 && forward <= SOS_Q16_ONE, "compatibility is a share in [0, ONE]");
    }

    /* Similar openness and agreeableness help; neuroticism hurts. */
    SetAll(&a, HALF);
    const SosQ16 kindred = SosPersonalityCompatibility(&a, &a);
    b = a;
    b.factor[SOS_PERSONALITY_OPENNESS] = 0;
    Expect(SosPersonalityCompatibility(&a, &b) < kindred, "a different openness gets on worse");
    b = a;
    b.factor[SOS_PERSONALITY_AGREEABLENESS] = SOS_Q16_ONE;
    Expect(SosPersonalityCompatibility(&a, &b) < kindred,
           "a different agreeableness gets on worse");
    b = a;
    b.factor[SOS_PERSONALITY_NEUROTICISM] = SOS_Q16_ONE;
    Expect(SosPersonalityCompatibility(&a, &b) < kindred, "a higher neuroticism gets on worse");

    Expect(SosPersonalityCompatibility(NULL, &b) == 0 && SosPersonalityCompatibility(&a, NULL) == 0,
           "no one is no compatibility");
}

static void TestTendency(void)
{
    SosPersonality neutral;
    SetAll(&neutral, HALF);
    const SosPersonality varied = {{0, SOS_Q16_ONE, HALF, 10000, 60000}};

    uint32_t distinct = 0u;
    SosQ16 seen[128];
    for (uint32_t id = 0u; id < 128u; ++id)
    {
        const SosQ16 first = SosPersonalityTendency(&varied, id);
        Expect(first >= 0 && first <= SOS_Q16_ONE, "a tendency is a share in [0, ONE]");
        Expect(SosPersonalityTendency(&varied, id) == first, "a tendency replays");
        Expect(SosPersonalityTendency(&neutral, id) == HALF,
               "averages are neutral to any behaviour");
        seen[id] = first;
        bool duplicate = false;
        for (uint32_t earlier = 0u; earlier < id; ++earlier)
        {
            duplicate = duplicate || seen[earlier] == first;
        }
        distinct += duplicate ? 0u : 1u;
    }
    Expect(distinct > 100u, "behaviour ids give many different tendencies");

    /* Extreme personalities stay in range for any id. */
    const SosPersonality extreme = {{SOS_Q16_ONE, 0, SOS_Q16_ONE, 0, SOS_Q16_ONE}};
    for (uint32_t id = 0u; id < 50u; ++id)
    {
        const SosQ16 tendency = SosPersonalityTendency(&extreme, id);
        Expect(tendency >= 0 && tendency <= SOS_Q16_ONE, "an extreme person still leans in range");
    }

    Expect(SosPersonalityTendency(NULL, 7u) == 0, "no one has no tendencies");
}

static uint64_t Fingerprint(void)
{
    const SosQ16 inherited[SOS_PERSONALITY_FACTOR_COUNT] = {0, SOS_Q16_ONE, HALF, 10000, 60000};
    SosPersonality person;
    SosPersonality other;
    SosPersonalityFrom(inherited, SEED, 12345u, &person);
    SosPersonalityFrom(NULL, SEED, 999u, &other);
    uint64_t hash = 0u;
    for (uint32_t factor = 0u; factor < SOS_PERSONALITY_FACTOR_COUNT; ++factor)
    {
        hash = SosHashCombine(hash, (uint64_t)(uint32_t)person.factor[factor]);
    }
    hash = SosHashCombine(hash, (uint64_t)(uint32_t)SosPersonalityCompatibility(&person, &other));
    hash = SosHashCombine(hash, (uint64_t)(uint32_t)SosPersonalityTendency(&person, 42u));
    return hash;
}

/* Fixed points of the deterministic arithmetic. Every platform and compiler
 * must build exactly this person and score them the same. */
static void TestGoldenValues(void)
{
    const uint64_t expected = UINT64_C(0xE45484D4EC4ACF2F);
    const uint64_t actual = Fingerprint();
    if (actual != expected)
    {
        fprintf(stderr, "personality fingerprint: 0x%016" PRIX64 "\n", actual);
        Expect(0, "the reference person matches");
    }
}

int main(void)
{
    TestFrom();
    TestShift();
    TestCompatibility();
    TestTendency();
    TestGoldenValues();
    return 0;
}
