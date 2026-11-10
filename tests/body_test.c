#include "sim/body.h"
#include "sim/calendar.h"
#include "sim/random.h"
#include "sim/space.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define YEARS(count) ((int64_t)(count) * SOS_TICKS_PER_YEAR)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "body test failed: %s\n", message);
        exit(1);
    }
}

/* A Q16 fraction from a whole percentage, so the test reads like the curves. */
static SosQ16 Percent(int value)
{
    return (SosQ16)(SOS_Q16_ONE * value / 100);
}

/* How far a body has grown toward its adult height, at the given age. */
static SosQ16 AdultFraction(const SosBodyPotential *potential, SosSex sex, int64_t ageTicks)
{
    SosBody young = {0};
    SosBody adult = {0};
    Expect(SosBodyAt(potential, sex, ageTicks, &young) &&
               SosBodyAt(potential, sex, YEARS(25), &adult),
           "two bodies to compare");
    return (SosQ16)((int64_t)young.heightMm * SOS_Q16_ONE / adult.heightMm);
}

static void TestGrowthAndAgeing(void)
{
    const SosBodyPotential potential = {Percent(50), Percent(50), Percent(50), Percent(50)};
    SosBody baby = {0};
    SosBody child = {0};
    SosBody adult = {0};
    SosBody old = {0};
    Expect(SosBodyAt(&potential, SOS_SEX_MALE, 0, &baby), "a newborn male has a body");
    Expect(SosBodyAt(&potential, SOS_SEX_MALE, YEARS(6), &child), "a boy has a body");
    Expect(SosBodyAt(&potential, SOS_SEX_MALE, YEARS(25), &adult), "a man has a body");
    Expect(SosBodyAt(&potential, SOS_SEX_MALE, YEARS(70), &old), "an old man has a body");
    Expect(baby.heightMm < child.heightMm && child.heightMm < adult.heightMm,
           "a male grows from birth to adulthood");
    Expect(baby.massGrams < child.massGrams && child.massGrams < adult.massGrams,
           "mass follows height through growth");
    Expect(adult.strength > old.strength && adult.speed > old.speed,
           "strength and speed decline in old age");
}

static void ExpectMonotone(SosSex sex)
{
    const SosBodyPotential potential = {Percent(85), Percent(50), Percent(50), Percent(50)};
    int32_t previousHeight = -1;
    int32_t previousMass = -1;
    for (int year = 0; year <= 25; ++year)
    {
        SosBody body = {0};
        Expect(SosBodyAt(&potential, sex, YEARS(year), &body), "a body exists each year");
        Expect(body.heightMm >= previousHeight, "growth never reverses");
        Expect(body.massGrams >= previousMass, "mass never falls while growing");
        previousHeight = body.heightMm;
        previousMass = body.massGrams;
    }
}

static void TestMonotoneGrowth(void)
{
    ExpectMonotone(SOS_SEX_FEMALE);
    ExpectMonotone(SOS_SEX_MALE);
}

static void TestSexDifference(void)
{
    const SosBodyPotential tallest = {SOS_Q16_ONE, SOS_Q16_ONE, SOS_Q16_ONE, SOS_Q16_ONE};
    const SosBodyPotential shortest = {0, 0, 0, 0};
    const SosBodyPotential median = {Percent(50), Percent(50), Percent(50), Percent(50)};
    SosBody tallMan = {0};
    SosBody tallWoman = {0};
    SosBody shortWoman = {0};
    SosBody medianMan = {0};
    SosBody medianWoman = {0};
    Expect(SosBodyAt(&tallest, SOS_SEX_MALE, YEARS(25), &tallMan) &&
               SosBodyAt(&tallest, SOS_SEX_FEMALE, YEARS(25), &tallWoman) &&
               SosBodyAt(&shortest, SOS_SEX_FEMALE, YEARS(25), &shortWoman),
           "adults of both sexes exist");
    Expect(tallMan.heightMm == 1950 && shortWoman.heightMm == 1500,
           "adult height spans about 150 to 195 cm");
    Expect(SosBodyAt(&median, SOS_SEX_MALE, YEARS(25), &medianMan) &&
               SosBodyAt(&median, SOS_SEX_FEMALE, YEARS(25), &medianWoman),
           "median adults of both sexes exist");
    Expect(medianMan.heightMm == 1750 && medianWoman.heightMm == 1620,
           "the median man is about 175 cm and the median woman about 162 cm");
    Expect(tallMan.heightMm > tallWoman.heightMm, "men stand taller at the same potential");
    Expect(AdultFraction(&tallest, SOS_SEX_FEMALE, YEARS(12)) >
               AdultFraction(&tallest, SOS_SEX_MALE, YEARS(12)),
           "girls finish their spurt earlier than boys");
}

static void TestDeclineAfterForty(void)
{
    const SosBodyPotential peak = {Percent(50), SOS_Q16_ONE, SOS_Q16_ONE, SOS_Q16_ONE};
    SosBody atThirty = {0};
    SosBody atForty = {0};
    SosBody afterForty = {0};
    SosBody atEighty = {0};
    SosBody ancient = {0};
    Expect(SosBodyAt(&peak, SOS_SEX_MALE, YEARS(30), &atThirty) &&
               SosBodyAt(&peak, SOS_SEX_MALE, YEARS(40), &atForty) &&
               SosBodyAt(&peak, SOS_SEX_MALE, YEARS(41), &afterForty) &&
               SosBodyAt(&peak, SOS_SEX_MALE, YEARS(80), &atEighty) &&
               SosBodyAt(&peak, SOS_SEX_MALE, YEARS(500), &ancient),
           "bodies of every age exist");
    Expect(atThirty.strength == atForty.strength, "strength holds on the plateau");
    Expect(afterForty.strength < atForty.strength, "strength falls after forty");
    Expect(atEighty.strength < afterForty.strength, "strength falls further by eighty");
    Expect(ancient.strength <= atEighty.strength, "strength never rises again");
    Expect(afterForty.speed < atForty.speed && atEighty.speed < afterForty.speed,
           "speed falls after forty as well");
    Expect(SosBodyWalkSpeed(&atForty) > SosBodyWalkSpeed(&atEighty),
           "an old body walks slower than an adult");
    Expect(atEighty.heightMm == atForty.heightMm, "height stays once adult");
}

static void TestLocomotion(void)
{
    const SosBodyPotential fast = {Percent(50), SOS_Q16_ONE, SOS_Q16_ONE, SOS_Q16_ONE};
    const SosBodyPotential slow = {Percent(50), 0, 0, 0};
    const SosBodyPotential median = {Percent(50), Percent(50), Percent(50), Percent(50)};
    SosBody adult = {0};
    SosBody child = {0};
    SosBody newborn = {0};
    SosBody slowest = {0};
    SosBody average = {0};
    const SosBody still = {0};
    Expect(SosBodyAt(&fast, SOS_SEX_MALE, YEARS(25), &adult) &&
               SosBodyAt(&fast, SOS_SEX_MALE, YEARS(5), &child) &&
               SosBodyAt(&fast, SOS_SEX_MALE, 0, &newborn) &&
               SosBodyAt(&slow, SOS_SEX_MALE, YEARS(25), &slowest) &&
               SosBodyAt(&median, SOS_SEX_FEMALE, YEARS(25), &average),
           "bodies to move exist");
    Expect(SosBodyWalkSpeed(&adult) > SosBodyWalkSpeed(&child) &&
               SosBodyWalkSpeed(&child) > SosBodyWalkSpeed(&newborn),
           "a newborn crawls and a five-year-old walks slower than an adult");
    Expect(SosBodyWalkSpeed(&newborn) * 4 < SosBodyWalkSpeed(&adult),
           "a newborn is far slower than an adult");
    /* The adult potential spans 1.1 m/s for the slowest to 1.7 m/s for the
     * fastest, so the median adult walks at the 1.4 m/s beings.c uses. */
    Expect(SosBodyWalkSpeed(&slowest) == SOS_METRES(11) / 10, "the slowest adult walks at 1.1 m/s");
    Expect(SosBodyWalkSpeed(&average) == SOS_METRES(14) / 10, "a median adult walks at 1.4 m/s");
    Expect(SosBodyWalkSpeed(&adult) == SOS_METRES(17) / 10, "the fastest adult walks at 1.7 m/s");
    Expect(SosBodyWalkSpeed(&still) == 0, "zero speed stands still");
    Expect(SosBodyCarryGrams(&adult) > SosBodyCarryGrams(&slowest), "a strong body carries more");
    Expect(SosBodyCarryGrams(&slowest) == 0, "a strengthless body carries nothing");
    Expect(SosBodyWalkSpeed(NULL) == 0 && SosBodyCarryGrams(NULL) == 0,
           "a missing body walks and carries nothing");
}

static void TestEdges(void)
{
    const SosBodyPotential potential = {SOS_Q16_ONE, SOS_Q16_ONE, SOS_Q16_ONE, SOS_Q16_ONE};
    SosBody body = {0};
    Expect(!SosBodyAt(NULL, SOS_SEX_MALE, 0, &body), "no potential, no body");
    Expect(!SosBodyAt(&potential, SOS_SEX_MALE, -1, &body), "a negative age has no body");
    Expect(!SosBodyAt(&potential, SOS_SEX_MALE, 0, NULL), "without an output there is no body");

    SosBody ancient = {0};
    Expect(SosBodyAt(&potential, SOS_SEX_MALE, INT64_MAX, &ancient),
           "an ancient being still has a body");
    Expect(ancient.heightMm > 0 && ancient.strength > 0, "ancient values stay sane");

    /* Out-of-range potentials clamp instead of overflowing the body. */
    const SosBodyPotential odd = {-SOS_Q16_ONE, -1, 5 * SOS_Q16_ONE, INT32_MIN};
    SosBody clamped = {0};
    Expect(SosBodyAt(&odd, SOS_SEX_FEMALE, YEARS(25), &clamped),
           "odd potentials still make a body");
    Expect(clamped.heightMm == 1500 && clamped.strength == 0, "negative potentials clamp to zero");
    Expect(SosBodyWalkSpeed(&clamped) == SOS_METRES(11) / 10,
           "a negative speed potential clamps to the slowest adult");
    Expect(clamped.endurance == SOS_Q16_ONE, "an excessive potential clamps to one");
}

static void TestDeterminism(void)
{
    const SosBodyPotential potential = {Percent(33), Percent(44), Percent(55), Percent(66)};
    for (int64_t age = 0; age <= YEARS(3); age += SOS_TICKS_PER_DAY)
    {
        SosBody first = {0};
        SosBody second = {0};
        Expect(SosBodyAt(&potential, SOS_SEX_FEMALE, age, &first) &&
                   SosBodyAt(&potential, SOS_SEX_FEMALE, age, &second),
               "two bodies of one age exist");
        Expect(first.heightMm == second.heightMm && first.massGrams == second.massGrams &&
                   first.strength == second.strength && first.endurance == second.endurance &&
                   first.speed == second.speed,
               "the same inputs give the same body");
    }
}

/* One fixed body. Every platform and compiler must produce exactly these
 * numbers. */
static void TestGolden(void)
{
    const SosBodyPotential potential = {Percent(72), Percent(64), Percent(58), Percent(66)};
    SosBody body = {0};
    Expect(SosBodyAt(&potential, SOS_SEX_MALE, YEARS(35) + 100 * SOS_TICKS_PER_DAY, &body),
           "the golden body exists");
    uint64_t fingerprint = SosHashCombine(0u, (uint64_t)(uint32_t)body.heightMm);
    fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)body.massGrams);
    fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)body.strength);
    fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)body.endurance);
    fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)body.speed);
    const uint64_t expected = UINT64_C(0x412567F7A7582270);
    if (fingerprint != expected)
    {
        fprintf(stderr,
                "body fingerprint: 0x%016" PRIX64 " (%d mm, %d g, %d %d %d; walk %" PRId64
                ", carry %d)\n",
                fingerprint, body.heightMm, body.massGrams, body.strength, body.endurance,
                body.speed, SosBodyWalkSpeed(&body), SosBodyCarryGrams(&body));
        Expect(0, "the body matches the reference");
    }
}

int main(void)
{
    TestGrowthAndAgeing();
    TestMonotoneGrowth();
    TestSexDifference();
    TestDeclineAfterForty();
    TestLocomotion();
    TestEdges();
    TestDeterminism();
    TestGolden();
    return 0;
}
