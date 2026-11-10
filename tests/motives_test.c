#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/motives.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "motives test failed: %s\n", message);
        exit(1);
    }
}

static void TestGrowth(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives m;
    SosMotivesInit(&m, 0u);
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    const uint32_t companionship = SOS_MOTIVE_COMPANIONSHIP;
    for (uint32_t motive = SOS_MOTIVE_WARMTH; motive < SOS_MOTIVE_COUNT; ++motive)
    {
        Expect(SosMotivesLevel(&m, &rates, motive, 0u) == 0, "a fresh drive is satisfied");
    }
    Expect(SosMotivesLevel(&m, &rates, companionship, day / 2u) == SOS_Q16_ONE / 2,
           "a drive is half full at half its period");
    Expect(SosMotivesLevel(&m, &rates, companionship, day) == SOS_Q16_ONE,
           "a drive fills over its period");
    Expect(SosMotivesLevel(&m, &rates, companionship, 2u * day) == SOS_Q16_ONE,
           "a drive stays unbearable");
    Expect(SosMotivesLevel(&m, &rates, companionship, day / 2u) == SOS_Q16_ONE / 2,
           "reading a level does not change it");
    SosQ16 previous = 0;
    for (uint64_t tick = 0u; tick <= day; tick += day / 8u)
    {
        const SosQ16 level = SosMotivesLevel(&m, &rates, companionship, tick);
        Expect(level >= previous, "a drive never falls with time");
        previous = level;
    }
    /* NULL rates are neutral personality. */
    Expect(SosMotivesLevel(&m, NULL, companionship, day) == SOS_Q16_ONE, "null rates are neutral");
}

static void TestPersonality(void)
{
    SosMotiveRates rates = SosMotiveRatesNeutral();
    rates.scale[SOS_MOTIVE_COMPANIONSHIP] = 2 * SOS_Q16_ONE;
    rates.scale[SOS_MOTIVE_CURIOSITY] = 0;
    SosMotives m;
    SosMotivesInit(&m, 0u);
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_COMPANIONSHIP, day / 2u) == SOS_Q16_ONE,
           "extraversion fills companionship twice as fast");
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_CURIOSITY, 10u * day) == 0,
           "a disabled drive never grows");

    /* An extreme multiplier is clamped to the documented maximum, and a
     * negative one to zero, as the shared rates are read. */
    rates.scale[SOS_MOTIVE_WARMTH] = INT32_MAX;
    rates.scale[SOS_MOTIVE_SAFETY] = -SOS_Q16_ONE;
    rates.scale[SOS_MOTIVE_COMPANIONSHIP] = SOS_Q16_ONE;
    rates.scale[SOS_MOTIVE_CURIOSITY] = SOS_Q16_ONE;
    SosMotiveRates clamped = rates;
    clamped.scale[SOS_MOTIVE_WARMTH] = SOS_MOTIVE_SCALE_MAX;
    clamped.scale[SOS_MOTIVE_SAFETY] = 0;
    for (uint64_t tick = 0u; tick <= (uint64_t)SOS_TICKS_PER_HOUR; tick += 300u)
    {
        Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_WARMTH, tick) ==
                   SosMotivesLevel(&m, &clamped, SOS_MOTIVE_WARMTH, tick),
               "an oversized multiplier is clamped");
        Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_SAFETY, tick) == 0,
               "a negative multiplier is clamped to zero");
    }
}

static void TestSatisfy(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives m;
    SosMotivesInit(&m, 0u);
    const uint64_t hour = (uint64_t)SOS_TICKS_PER_HOUR;
    /* Warmth fills in four hours, so it is half full after two. */
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_WARMTH, 2u * hour) == SOS_Q16_ONE / 2,
           "warmth is half full at half its period");
    /* Every drive shares one counter, so satisfying companionship must keep
     * the warmth grown so far instead of losing it. */
    Expect(SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_COMPANIONSHIP, SOS_Q16_ONE, 2u * hour),
           "a tracked drive can be satisfied");
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_WARMTH, 2u * hour) == SOS_Q16_ONE / 2,
           "satisfying one drive keeps the others current");
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_WARMTH, 3u * hour) == 3 * SOS_Q16_ONE / 4,
           "the other drives keep growing from where they were");
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_COMPANIONSHIP, 3u * hour) > 0,
           "a satisfied drive fills again with time");
    /* A tick before the counter cannot turn time back. */
    Expect(SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_WARMTH, SOS_Q16_ONE / 2, 0u),
           "satisfying in the past is accepted");
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_WARMTH, 3u * hour) == SOS_Q16_ONE / 4,
           "the counter never moves backwards");
    /* Over-satisfying stops at zero. */
    Expect(SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_WARMTH, SOS_Q16_ONE, 3u * hour),
           "overshoot is allowed");
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_WARMTH, 3u * hour) == 0,
           "a drive never goes below zero");
    /* Touching one drive leaves the others on their natural curve. Rebasing to
     * the shared counter truncates, so a unit can be lost. */
    SosMotives untouched;
    SosMotivesInit(&untouched, 0u);
    const SosQ16 touched = SosMotivesLevel(&m, &rates, SOS_MOTIVE_ESTEEM, 3u * hour);
    const SosQ16 natural = SosMotivesLevel(&untouched, &rates, SOS_MOTIVE_ESTEEM, 3u * hour);
    Expect(touched > 0 && touched <= natural && natural - touched <= 1,
           "an untouched drive grows as if nothing else had been satisfied");
}

/* Satisfying one drive repeatedly must not slow the others: the fraction a
 * drive has already earned has to survive the rebase that every call performs.
 * Calls are dense over the first day, where the fastest drives fill and the
 * carry matters most, then coarser but still many times per growth unit; a
 * `span` long enough to saturate every drive and one short enough to catch
 * them mid-growth both have to match a being nobody touched. */
static void ExpectUntouchedGrowthMatches(uint64_t span)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives touched;
    SosMotives untouched;
    SosMotivesInit(&touched, 0u);
    SosMotivesInit(&untouched, 0u);
    const uint64_t day = (uint64_t)SOS_TICKS_PER_DAY;
    const uint64_t dense = span < day ? span : day;
    /* Eight seconds: still a small fraction of every drive's period, so the
     * carried fraction keeps being split and rejoined long after the first
     * day, while the call count stays far inside the test's time budget. */
    const uint64_t stride = (uint64_t)SOS_TICKS_PER_HOUR / 15u;
    bool accepted = true;
    uint64_t tick = 1u;
    for (; tick <= dense; ++tick)
    {
        accepted =
            SosMotivesSatisfy(&touched, &rates, SOS_MOTIVE_WARMTH, SOS_Q16_ONE, tick) && accepted;
    }
    for (; tick <= span; tick += stride)
    {
        accepted =
            SosMotivesSatisfy(&touched, &rates, SOS_MOTIVE_WARMTH, SOS_Q16_ONE, tick) && accepted;
    }
    Expect(accepted, "every frequent satisfaction is accepted");
    for (uint32_t motive = SOS_MOTIVE_WARMTH + 1u; motive < SOS_MOTIVE_COUNT; ++motive)
    {
        Expect(SosMotivesLevel(&touched, &rates, motive, span) ==
                   SosMotivesLevel(&untouched, &rates, motive, span),
               "satisfying one drive often leaves the others exact");
    }
    Expect(SosMotivesLevel(&touched, &rates, SOS_MOTIVE_WARMTH, span) < SOS_Q16_ONE,
           "the satisfied drive is held down");
    Expect(SosMotivesLevel(&untouched, &rates, SOS_MOTIVE_WARMTH, span) > 0,
           "an untouched drive still grows");
}

static void TestFrequentSatisfy(void)
{
    /* Two hours leaves the slower drives still filling. */
    ExpectUntouchedGrowthMatches(2u * (uint64_t)SOS_TICKS_PER_HOUR);
    /* Thirty days leaves every drive saturated. */
    ExpectUntouchedGrowthMatches(30u * (uint64_t)SOS_TICKS_PER_DAY);
}

/* Two beings can read the same level now and still part ways: the one that has
 * already earned a larger fraction of its next unit fills sooner. The hash
 * must fold that fraction, or a replay comparing hashes would miss the split. */
static void TestHashCoversCarry(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives behind;
    SosMotives ahead;
    SosMotivesInit(&behind, 0u);
    SosMotivesInit(&ahead, 0u);
    ahead.carry[SOS_MOTIVE_COMPANIONSHIP] = 1u;
    behind.carry[SOS_MOTIVE_COMPANIONSHIP] = 7u;
    Expect(SosMotivesLevel(&ahead, &rates, SOS_MOTIVE_COMPANIONSHIP, 0u) ==
               SosMotivesLevel(&behind, &rates, SOS_MOTIVE_COMPANIONSHIP, 0u),
           "the carried fraction does not change the level at the same tick");
    Expect(SosMotivesHash(&ahead, &rates, 0u) != SosMotivesHash(&behind, &rates, 0u),
           "a different carried fraction hashes differently");

    /* Equal carries still hash equally, so the fold does not invent a split. */
    SosMotives copy = ahead;
    Expect(SosMotivesHash(&copy, &rates, 0u) == SosMotivesHash(&ahead, &rates, 0u),
           "an equal carried fraction hashes equally");
}

static void TestSatisfyRejects(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives m;
    SosMotivesInit(&m, 0u);
    const uint64_t tick = (uint64_t)SOS_TICKS_PER_DAY;
    const uint64_t before = SosMotivesHash(&m, &rates, tick);
    Expect(!SosMotivesSatisfy(NULL, &rates, SOS_MOTIVE_WARMTH, SOS_Q16_ONE, tick),
           "null has nothing to satisfy");
    Expect(!SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_NONE, SOS_Q16_ONE, tick),
           "none is not a drive");
    Expect(!SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_COUNT, SOS_Q16_ONE, tick),
           "the count is not a drive");
    Expect(!SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_COUNT + 3u, SOS_Q16_ONE, tick),
           "a drive beyond the enum is not tracked");
    Expect(!SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_WARMTH, 0, tick),
           "satisfying by nothing is ignored");
    Expect(!SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_WARMTH, -SOS_Q16_ONE, tick),
           "negative satisfaction is ignored");
    Expect(SosMotivesHash(&m, &rates, tick) == before, "a rejected satisfaction changes nothing");
}

static void TestUrgency(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives m;
    SosMotivesInit(&m, 0u);
    Expect(SosMotivesUrgency(&m, &rates, SOS_MOTIVE_WARMTH, 0u) == 0,
           "a satisfied drive is not urgent");
    m.level[SOS_MOTIVE_WARMTH] = SOS_Q16_ONE / 2;
    Expect(SosMotivesUrgency(&m, &rates, SOS_MOTIVE_WARMTH, 0u) == SOS_Q16_ONE / 4,
           "urgency squares the level");
    m.level[SOS_MOTIVE_WARMTH] = SOS_Q16_ONE;
    Expect(SosMotivesUrgency(&m, &rates, SOS_MOTIVE_WARMTH, 0u) == SOS_Q16_ONE,
           "an unbearable drive is fully urgent");
    /* Convex: one urgent drive outweighs two half-full ones. */
    const SosQ16 half = SosQ16Mul(SOS_Q16_ONE / 2, SOS_Q16_ONE / 2);
    Expect(SosMotivesUrgency(&m, &rates, SOS_MOTIVE_WARMTH, 0u) > 2 * half,
           "urgency favours the urgent drive");
    Expect(SosMotivesUrgency(NULL, &rates, SOS_MOTIVE_WARMTH, 0u) == 0, "null is never urgent");
    Expect(SosMotivesUrgency(&m, &rates, SOS_MOTIVE_NONE, 0u) == 0, "none is never urgent");
}

static void TestMostUrgent(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives m;
    SosMotivesInit(&m, 0u);
    Expect(SosMotivesMostUrgent(&m, &rates, 0u) == SOS_MOTIVE_WARMTH, "ties go in enum order");
    m.level[SOS_MOTIVE_PURPOSE] = SOS_Q16_ONE;
    m.level[SOS_MOTIVE_CURIOSITY] = SOS_Q16_ONE / 2;
    Expect(SosMotivesMostUrgent(&m, &rates, 0u) == SOS_MOTIVE_PURPOSE, "the fullest drive wins");
    m.level[SOS_MOTIVE_ESTEEM] = SOS_Q16_ONE;
    Expect(SosMotivesMostUrgent(&m, &rates, 0u) == SOS_MOTIVE_ESTEEM,
           "equal drives go in enum order");
    Expect(SosMotivesMostUrgent(NULL, &rates, 0u) == SOS_MOTIVE_NONE, "null has no urgent drive");
}

static void TestChoose(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives m;
    SosMotivesInit(&m, 0u);
    m.level[SOS_MOTIVE_WARMTH] = SOS_Q16_ONE;   /* urgency one */
    m.level[SOS_MOTIVE_PLAY] = SOS_Q16_ONE / 2; /* urgency a quarter */

    SosMotiveOption options[2] = {0};
    options[0].pairs[0] = (SosMotivePair){.motive = SOS_MOTIVE_WARMTH, .satisfaction = SOS_Q16_ONE};
    options[1].pairs[0] = (SosMotivePair){.motive = SOS_MOTIVE_PLAY, .satisfaction = SOS_Q16_ONE};
    Expect(SosMotivesChoose(&m, &rates, 0u, options, 2u) == 0, "the more urgent gain wins");
    Expect(SosMotivesChoose(&m, &rates, 0u, options, 2u) == 0, "the choice replays");

    options[0].cost = SOS_Q16_ONE;
    Expect(SosMotivesChoose(&m, &rates, 0u, options, 2u) == 1, "cost steers the choice");
    options[0].cost = 0;

    options[1].pairs[0] = options[0].pairs[0];
    Expect(SosMotivesChoose(&m, &rates, 0u, options, 2u) == 0,
           "an equal option at a higher index loses");

    /* Satisfying several motives in one option adds up. */
    SosMotiveOption many = {0};
    many.pairs[0] = (SosMotivePair){.motive = SOS_MOTIVE_WARMTH, .satisfaction = SOS_Q16_ONE / 2};
    many.pairs[1] = (SosMotivePair){.motive = SOS_MOTIVE_PLAY, .satisfaction = SOS_Q16_ONE};
    SosMotiveOption one = {0};
    one.pairs[0] = (SosMotivePair){.motive = SOS_MOTIVE_PLAY, .satisfaction = SOS_Q16_ONE};
    const SosMotiveOption two[2] = {many, one};
    Expect(SosMotivesChoose(&m, &rates, 0u, two, 2u) == 0, "several motives in one option add up");

    /* A satisfaction of an untracked id adds nothing to any option. */
    SosMotiveOption untracked[2] = {0};
    untracked[0].pairs[0] =
        (SosMotivePair){.motive = SOS_MOTIVE_COUNT + 5u, .satisfaction = SOS_Q16_ONE};
    untracked[1].pairs[0] = (SosMotivePair){.motive = SOS_MOTIVE_PLAY, .satisfaction = SOS_Q16_ONE};
    Expect(SosMotivesChoose(&m, &rates, 0u, untracked, 2u) == 1,
           "an untracked motive is worth nothing");

    Expect(SosMotivesChoose(NULL, &rates, 0u, options, 2u) == -1, "null turns down every option");
    Expect(SosMotivesChoose(&m, &rates, 0u, NULL, 2u) == -1, "no options means no choice");
    Expect(SosMotivesChoose(&m, &rates, 0u, options, 0u) == -1, "an empty list has no choice");
}

static void TestUntrackedDrives(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives m;
    SosMotivesInit(&m, 0u);
    const uint32_t extra = SOS_MOTIVE_COUNT + 7u;
    const uint64_t year = (uint64_t)SOS_TICKS_PER_YEAR;
    /* Nothing beyond the enum exists: no level, no growth, no urgency, no way
     * to satisfy it, however much time passes. */
    Expect(SosMotivesLevel(&m, &rates, extra, year) == 0, "an id beyond the enum has no level");
    Expect(SosMotivesLevel(&m, &rates, extra, UINT64_MAX) == 0, "nor after any wait");
    Expect(SosMotivesUrgency(&m, &rates, extra, year) == 0, "nor any urgency");
    Expect(!SosMotivesSatisfy(&m, &rates, extra, SOS_Q16_ONE, year),
           "an id beyond the enum cannot be satisfied");
}

static void TestCompactState(void)
{
    /* Growing drives lazily from one tick must cost a million beings little:
     * nine Q16 levels, a remainder of the same small width each, and a single
     * counter, no copied personality. */
    Expect(sizeof(SosMotives) <= 96u, "the per-being value stays compact");
    Expect(sizeof(((const SosMotives *)0)->carry[0]) <= sizeof(SosQ16),
           "the fractional remainder is no wider than a level");
}

static void TestExtremes(void)
{
    const SosMotiveRates rates = SosMotiveRatesNeutral();
    SosMotives m;
    SosMotivesInit(&m, 0u);
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_WARMTH, UINT64_MAX) == SOS_Q16_ONE,
           "an extreme tick saturates a drive");
    Expect(SosMotivesUrgency(&m, &rates, SOS_MOTIVE_WARMTH, UINT64_MAX) == SOS_Q16_ONE,
           "an extreme tick saturates urgency");
    Expect(SosMotivesHash(&m, &rates, UINT64_MAX) != 0u, "an extreme tick still hashes");
    Expect(SosMotivesSatisfy(&m, &rates, SOS_MOTIVE_WARMTH, SOS_Q16_ONE / 2, UINT64_MAX),
           "an extreme tick can be satisfied");
    Expect(SosMotivesLevel(&m, &rates, SOS_MOTIVE_WARMTH, UINT64_MAX) == SOS_Q16_ONE / 2,
           "taking an amount off a saturated drive leaves the rest");
    /* A null object is left alone and fails safely. */
    SosMotivesInit(NULL, 0u);
    Expect(SosMotivesHash(NULL, &rates, 0u) == 0u, "null hashes to zero");
    Expect(SosMotivesLevel(NULL, &rates, SOS_MOTIVE_WARMTH, 0u) == 0, "null has no level");
    Expect(SosMotivesMostUrgent(NULL, &rates, 0u) == SOS_MOTIVE_NONE, "null has no urgent drive");
    Expect(SosMotivesChoose(NULL, &rates, 0u, NULL, 0u) == -1, "null turns down every option");
}

/* A fixed personality, used for determinism and the golden value. */
static SosMotiveRates ScriptedRates(void)
{
    SosMotiveRates rates = SosMotiveRatesNeutral();
    rates.scale[SOS_MOTIVE_COMPANIONSHIP] = SOS_Q16_ONE + SOS_Q16_ONE / 2;
    rates.scale[SOS_MOTIVE_CURIOSITY] = SOS_Q16_ONE / 2;
    return rates;
}

/* A fixed history of one being, used for determinism and the golden value. */
static SosMotives Scripted(const SosMotiveRates *rates)
{
    SosMotives m;
    SosMotivesInit(&m, 0u);
    (void)SosMotivesSatisfy(&m, rates, SOS_MOTIVE_WARMTH, SOS_Q16_ONE / 3,
                            2u * (uint64_t)SOS_TICKS_PER_HOUR);
    (void)SosMotivesSatisfy(&m, rates, SOS_MOTIVE_COMPANIONSHIP, SOS_Q16_ONE / 4,
                            5u * (uint64_t)SOS_TICKS_PER_HOUR);
    (void)SosMotivesSatisfy(&m, rates, SOS_MOTIVE_PURPOSE, SOS_Q16_ONE / 2,
                            (uint64_t)SOS_TICKS_PER_DAY);
    return m;
}

static uint64_t Fingerprint(const SosMotives *m, const SosMotiveRates *rates, uint64_t tick)
{
    uint64_t fingerprint = SosMotivesHash(m, rates, tick);
    for (uint32_t motive = SOS_MOTIVE_WARMTH; motive < SOS_MOTIVE_COUNT; ++motive)
    {
        fingerprint = SosHashCombine(fingerprint,
                                     (uint64_t)(uint32_t)SosMotivesLevel(m, rates, motive, tick));
    }
    return fingerprint;
}

static void TestDeterminism(void)
{
    const SosMotiveRates rates = ScriptedRates();
    const SosMotives first = Scripted(&rates);
    const SosMotives second = Scripted(&rates);
    const uint64_t tick = 2u * (uint64_t)SOS_TICKS_PER_DAY;
    Expect(SosMotivesHash(&first, &rates, tick) == SosMotivesHash(&second, &rates, tick),
           "the same history hashes equally");
    const uint64_t before = SosMotivesHash(&first, &rates, tick);
    (void)SosMotivesLevel(&first, &rates, SOS_MOTIVE_WARMTH, tick);
    Expect(SosMotivesHash(&first, &rates, tick) == before, "reading a value does not change it");
    for (uint32_t motive = SOS_MOTIVE_WARMTH; motive < SOS_MOTIVE_COUNT; ++motive)
    {
        Expect(SosMotivesLevel(&first, &rates, motive, tick) ==
                   SosMotivesLevel(&second, &rates, motive, tick),
               "the same history gives the same levels");
    }
}

/* A fixed point of the drives. Every platform and compiler must reach it. */
static void TestGoldenValue(void)
{
    const SosMotiveRates rates = ScriptedRates();
    const SosMotives m = Scripted(&rates);
    const uint64_t tick = 2u * (uint64_t)SOS_TICKS_PER_DAY;
    const uint64_t fingerprint = Fingerprint(&m, &rates, tick);
    const uint64_t expected = UINT64_C(0xDBC66E073D295ABD);
    if (fingerprint != expected)
    {
        fprintf(stderr, "motive fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the drives match the reference");
    }
}

int main(void)
{
    TestGrowth();
    TestPersonality();
    TestSatisfy();
    TestFrequentSatisfy();
    TestHashCoversCarry();
    TestSatisfyRejects();
    TestUrgency();
    TestMostUrgent();
    TestChoose();
    TestUntrackedDrives();
    TestCompactState();
    TestExtremes();
    TestDeterminism();
    TestGoldenValue();
    return 0;
}
