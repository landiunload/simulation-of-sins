#include "sim/biology.h"
#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/lifecycle.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define YEAR SOS_TICKS_PER_YEAR
#define NEUTRAL (SOS_Q16_ONE / 2)
#define SEED UINT64_C(20261015)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "lifecycle test failed: %s\n", message);
        exit(1);
    }
}

static void TestStages(void)
{
    Expect(SosLifecycleStage(INT64_MIN) == SOS_LIFE_UNBORN, "a very negative age is unborn");
    Expect(SosLifecycleStage(-1) == SOS_LIFE_UNBORN, "a negative age is unborn");
    Expect(SosLifecycleStage(0) == SOS_LIFE_INFANT, "birth is an infant");
    Expect(SosLifecycleStage(2 * YEAR - 1) == SOS_LIFE_INFANT, "just under two is an infant");
    Expect(SosLifecycleStage(2 * YEAR) == SOS_LIFE_CHILD, "two is a child");
    Expect(SosLifecycleStage(12 * YEAR - 1) == SOS_LIFE_CHILD, "just under twelve is a child");
    Expect(SosLifecycleStage(12 * YEAR) == SOS_LIFE_ADOLESCENT, "twelve is an adolescent");
    Expect(SosLifecycleStage(16 * YEAR - 1) == SOS_LIFE_ADOLESCENT,
           "just under sixteen is an adolescent");
    Expect(SosLifecycleStage(16 * YEAR) == SOS_LIFE_ADULT, "sixteen is an adult");
    Expect(SosLifecycleStage(45 * YEAR - 1) == SOS_LIFE_ADULT, "just under forty-five is an adult");
    Expect(SosLifecycleStage(45 * YEAR) == SOS_LIFE_ELDER, "forty-five is an elder");
    Expect(SosLifecycleStage(INT64_MAX) == SOS_LIFE_ELDER, "an extreme age is an elder");
}

/* Fertility is a shape: nothing before puberty, a rise, a peak through the
 * twenties for women, a slow male decline, and nothing after the end. */
static void TestFertility(void)
{
    Expect(SosLifecycleFertility(SOS_SEX_FEMALE, -1) == 0, "an unborn woman is infertile");
    Expect(SosLifecycleFertility(SOS_SEX_MALE, -1) == 0, "an unborn man is infertile");
    Expect(SosLifecycleFertility(SOS_SEX_FEMALE, 10 * YEAR) == 0, "a girl is infertile");
    Expect(SosLifecycleFertility(SOS_SEX_FEMALE, 16 * YEAR) > 0, "a young woman is fertile");
    Expect(SosLifecycleFertility(SOS_SEX_FEMALE, 25 * YEAR) == SOS_Q16_ONE,
           "a woman peaks at full fertility");
    Expect(SosLifecycleFertility(SOS_SEX_FEMALE, 20 * YEAR) ==
               SosLifecycleFertility(SOS_SEX_FEMALE, 30 * YEAR),
           "the female peak is a plateau through the twenties");
    Expect(SosLifecycleFertility(SOS_SEX_FEMALE, 50 * YEAR) == 0, "a woman is infertile by fifty");
    Expect(SosLifecycleFertility(SOS_SEX_FEMALE, 60 * YEAR) == 0, "she stays infertile");

    int previous = SosLifecycleFertility(SOS_SEX_FEMALE, 12 * YEAR);
    for (int64_t age = 13 * YEAR; age <= 25 * YEAR; age += YEAR)
    {
        const int now = SosLifecycleFertility(SOS_SEX_FEMALE, age);
        Expect(now >= previous, "female fertility rises to its peak");
        previous = now;
    }
    for (int64_t age = 30 * YEAR; age <= 50 * YEAR; age += YEAR)
    {
        const int now = SosLifecycleFertility(SOS_SEX_FEMALE, age);
        Expect(now <= previous, "female fertility falls after its peak");
        previous = now;
    }

    Expect(SosLifecycleFertility(SOS_SEX_MALE, 14 * YEAR) == 0, "a boy is infertile");
    Expect(SosLifecycleFertility(SOS_SEX_MALE, 18 * YEAR) > 0, "a young man is fertile");
    Expect(SosLifecycleFertility(SOS_SEX_MALE, 85 * YEAR) == 0, "an old man is infertile");
    int malePrevious = SosLifecycleFertility(SOS_SEX_MALE, 18 * YEAR);
    for (int64_t age = 20 * YEAR; age <= 85 * YEAR; age += 5 * YEAR)
    {
        const int now = SosLifecycleFertility(SOS_SEX_MALE, age);
        Expect(now <= malePrevious, "male fertility declines");
        malePrevious = now;
    }

    Expect(SosLifecycleFertility((SosSex)7, 25 * YEAR) == 0, "an unknown sex is infertile");
    for (int64_t age = 0; age <= 120 * YEAR; age += 3 * YEAR)
    {
        const int female = SosLifecycleFertility(SOS_SEX_FEMALE, age);
        const int male = SosLifecycleFertility(SOS_SEX_MALE, age);
        Expect(female >= 0 && female <= SOS_Q16_ONE && male >= 0 && male <= SOS_Q16_ONE,
               "fertility stays a fraction");
    }
}

/* Mortality is a Gompertz-Makeham curve: an infant hump that falls, a long
 * safe childhood, an adulthood that about doubles every eight years. */
static void TestMortality(void)
{
    Expect(SosLifecycleMortality(-1, NEUTRAL) == 0, "an unborn being has no hazard");
    Expect(SosLifecycleMortality(0, NEUTRAL) > SosLifecycleMortality(YEAR, NEUTRAL),
           "the infant hump falls through the first year");
    Expect(SosLifecycleMortality(YEAR, NEUTRAL) > SosLifecycleMortality(5 * YEAR, NEUTRAL),
           "the hump is gone by childhood");
    Expect(SosLifecycleMortality(5 * YEAR, NEUTRAL) < SosLifecycleMortality(20 * YEAR, NEUTRAL),
           "mortality rises into adulthood");
    Expect(SosLifecycleMortality(20 * YEAR, NEUTRAL) < SosLifecycleMortality(40 * YEAR, NEUTRAL),
           "mortality keeps rising");
    Expect(SosLifecycleMortality(40 * YEAR, NEUTRAL) < SosLifecycleMortality(60 * YEAR, NEUTRAL),
           "mortality rises through old age");

    /* An eight-year step about doubles the adult hazard. */
    const int at40 = SosLifecycleMortality(40 * YEAR, NEUTRAL);
    const int at48 = SosLifecycleMortality(48 * YEAR, NEUTRAL);
    Expect(at48 > at40 * 17 / 10 && at48 < at40 * 205 / 100,
           "the adult hazard doubles about every eight years");

    const int robust = SosLifecycleMortality(50 * YEAR, 0);
    const int typical = SosLifecycleMortality(50 * YEAR, NEUTRAL);
    const int frail = SosLifecycleMortality(50 * YEAR, SOS_Q16_ONE);
    Expect(robust < typical && typical < frail, "frailty scales the hazard up");
    Expect(robust > typical / 2 - typical / 50 && robust < typical / 2 + typical / 50,
           "a robust being halves the hazard");
    Expect(frail > typical * 3 / 2 - typical / 50 && frail < typical * 3 / 2 + typical / 50,
           "a frail being raises it by half");
    Expect(SosLifecycleMortality(50 * YEAR, -1) == robust &&
               SosLifecycleMortality(50 * YEAR, INT32_MAX) == frail,
           "out-of-range frailty is clamped");
    Expect(SosLifecycleMortality(SosLifecycleMaxAgeTicks(), NEUTRAL) ==
               SosLifecycleMortality(INT64_MAX, NEUTRAL),
           "the hazard saturates at the maximum age");
}

static void TestSurvive(void)
{
    const int64_t year = YEAR;
    Expect(SosLifecycleSurvive(-1, (uint64_t)year, NEUTRAL) == SOS_Q16_ONE,
           "an unborn being cannot die");
    Expect(SosLifecycleSurvive(20 * year, 0u, NEUTRAL) == SOS_Q16_ONE,
           "a zero duration always survives");
    Expect(SosLifecycleSurvive(20 * year, (uint64_t)year, NEUTRAL) < SOS_Q16_ONE,
           "a year of adult life is not certain");
    Expect(SosLifecycleSurvive(20 * year, (uint64_t)year, NEUTRAL) >
               SosLifecycleSurvive(20 * year, (uint64_t)(2 * year), NEUTRAL),
           "a longer duration survives less");
    Expect(SosLifecycleSurvive(SosLifecycleMaxAgeTicks(), (uint64_t)year, NEUTRAL) == 0,
           "the oldest age cannot survive");
    Expect(SosLifecycleSurvive(0, (uint64_t)year, NEUTRAL) > 0 &&
               SosLifecycleSurvive(0, (uint64_t)year, NEUTRAL) < SOS_Q16_ONE,
           "a baby usually survives its first year but not always");

    /* Splitting a year into 365 days leaves the survival chance where it was:
     * the property that lets a step use any duration. */
    for (int64_t age = 0; age <= 80 * year; age += 20 * year)
    {
        const int64_t whole = SosLifecycleSurvive(age, (uint64_t)year, NEUTRAL);
        const int64_t daily = SosLifecycleSurvive(age, SOS_TICKS_PER_DAY, NEUTRAL);
        int64_t product = SOS_Q16_ONE;
        for (int day = 0; day < 365; ++day)
        {
            product = product * daily / SOS_Q16_ONE;
        }
        const int64_t difference = product > whole ? product - whole : whole - product;
        Expect(difference <= whole / 50, "a year equals 365 daily survivals");
    }
}

static void TestDies(void)
{
    SosRandom random = SosRandomStream(SEED, 1u);
    Expect(!SosLifecycleDies(NULL, 20 * YEAR, (uint64_t)YEAR, NEUTRAL), "no random, no death");
    Expect(!SosLifecycleDies(&random, -1, (uint64_t)YEAR, NEUTRAL), "an unborn being cannot die");
    Expect(!SosLifecycleDies(&random, 20 * YEAR, 0u, NEUTRAL), "a zero duration cannot kill");
    Expect(SosLifecycleDies(&random, SosLifecycleMaxAgeTicks(), (uint64_t)YEAR, NEUTRAL),
           "the oldest age always dies");

    /* The draw matches the survival chance it reports. */
    const int sampleCount = 20000;
    const int64_t age = 50 * YEAR;
    int died = 0;
    int dailyDied = 0;
    SosRandom yearly = SosRandomStream(SEED, 2u);
    SosRandom daily = SosRandomStream(SEED, 2u);
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        if (SosLifecycleDies(&yearly, age, (uint64_t)YEAR, NEUTRAL))
        {
            ++died;
        }
        int aliveThroughYear = 0;
        for (int day = 0; day < 365; ++day)
        {
            if (SosLifecycleDies(&daily, age, SOS_TICKS_PER_DAY, NEUTRAL))
            {
                aliveThroughYear = 1;
                break;
            }
        }
        dailyDied += aliveThroughYear;
    }
    const int chance = SOS_Q16_ONE - SosLifecycleSurvive(age, (uint64_t)YEAR, NEUTRAL);
    const int64_t observed = (int64_t)died * SOS_Q16_ONE;
    const int64_t expectedDeaths = (int64_t)chance * sampleCount;
    const int64_t chanceError =
        observed > expectedDeaths ? observed - expectedDeaths : expectedDeaths - observed;
    Expect(chanceError < (int64_t)2000 * sampleCount, "the yearly draw follows its chance");
    const int spread = died > dailyDied ? died - dailyDied : dailyDied - died;
    Expect(spread < sampleCount / 50, "a yearly draw matches 365 daily draws");

    /* Determinism: one stream replays exactly. */
    SosRandom first = SosRandomStream(SEED, 3u);
    SosRandom again = SosRandomStream(SEED, 3u);
    for (int step = 0; step < 500; ++step)
    {
        const int64_t at = (int64_t)(step % 90) * YEAR;
        Expect(SosLifecycleDies(&first, at, (uint64_t)YEAR, NEUTRAL) ==
                   SosLifecycleDies(&again, at, (uint64_t)YEAR, NEUTRAL),
               "the same stream replays the same deaths");
    }
}

/* A forager life table: about a third of children die, those who reach
 * adulthood often see sixty, and life expectancy at birth sits in the low
 * thirties. The stream is fixed, so the sample is identical everywhere. */
static void TestLifeExpectancy(void)
{
    const int sampleCount = 20000;
    const int64_t maxAge = SosLifecycleMaxAgeTicks();
    SosRandom random = SosRandomStream(SEED, 4u);
    int64_t totalAge = 0;
    int reachedAdult = 0;
    int reachedSixty = 0;
    int reachedSeventy = 0;
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        int64_t age = 0;
        while (age < maxAge && !SosLifecycleDies(&random, age, (uint64_t)YEAR, NEUTRAL))
        {
            age += YEAR;
        }
        totalAge += age;
        if (age >= 15 * YEAR)
        {
            ++reachedAdult;
            reachedSixty += age >= 60 * YEAR;
            reachedSeventy += age >= 70 * YEAR;
        }
    }
    const int64_t expectancy = totalAge / sampleCount;
    Expect(expectancy > 30 * YEAR && expectancy < 40 * YEAR,
           "life expectancy at birth is plausible for foragers");
    Expect(reachedAdult > sampleCount * 55 / 100 && reachedAdult < sampleCount * 72 / 100,
           "a forager childhood kills about a third");
    Expect(reachedSixty > reachedAdult * 30 / 100, "adults often see sixty");
    Expect(reachedSeventy > reachedAdult * 8 / 100, "and some see seventy");
}

/* Fixed points of the whole module. Every platform and compiler must produce
 * exactly these values. */
static uint64_t Fingerprint(void)
{
    uint64_t hash = 0u;
    hash = SosHashCombine(hash, (uint64_t)SosLifecycleGestationTicks());
    hash = SosHashCombine(hash, (uint64_t)SosLifecycleMaxAgeTicks());
    for (int64_t age = 0; age <= 100 * YEAR; age += 7 * YEAR)
    {
        hash = SosHashCombine(hash, (uint64_t)SosLifecycleStage(age));
        hash = SosHashCombine(hash, (uint64_t)SosLifecycleFertility(SOS_SEX_FEMALE, age));
        hash = SosHashCombine(hash, (uint64_t)SosLifecycleFertility(SOS_SEX_MALE, age));
        hash = SosHashCombine(hash, (uint64_t)SosLifecycleMortality(age, NEUTRAL));
        hash = SosHashCombine(hash, (uint64_t)SosLifecycleSurvive(age, (uint64_t)YEAR, NEUTRAL));
    }
    return hash;
}

static void TestGoldenValues(void)
{
    const uint64_t expected = UINT64_C(0x4392D5AE82C4724C);
    const uint64_t fingerprint = Fingerprint();
    if (fingerprint != expected)
    {
        fprintf(stderr, "lifecycle fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the life table matches the reference");
    }
}

int main(void)
{
    TestStages();
    TestFertility();
    TestMortality();
    TestSurvive();
    TestDies();
    TestLifeExpectancy();
    TestGoldenValues();
    return 0;
}
