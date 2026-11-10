#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/random.h"
#include "sim/sins.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEED UINT64_C(20261015)
#define SERIAL UINT64_C(7)

/* The fields of SosSinSituation, in declaration order, for the sweep below. */
enum
{
    INPUT_OPPORTUNITY = 0,
    INPUT_NEED,
    INPUT_PROVOCATION,
    INPUT_RIVAL,
    INPUT_WATCHED,
    INPUT_MOOD,
    INPUT_COUNT,
};

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "sins test failed: %s\n", message);
        exit(1);
    }
}

static void FillPredisposition(SosQ16 value, SosQ16 out[SOS_SIN_COUNT])
{
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        out[sin] = value;
    }
}

static void SetInput(SosSinSituation *situation, int field, SosQ16 value)
{
    switch (field)
    {
    case INPUT_OPPORTUNITY:
        situation->opportunity = value;
        break;
    case INPUT_NEED:
        situation->need = value;
        break;
    case INPUT_PROVOCATION:
        situation->provocation = value;
        break;
    case INPUT_RIVAL:
        situation->rivalAdvantage = value;
        break;
    case INPUT_WATCHED:
        situation->watched = value;
        break;
    case INPUT_MOOD:
        situation->mood = value;
        break;
    default:
        break;
    }
}

static void TestNames(void)
{
    static const char *const sins[SOS_SIN_COUNT] = {"pride",    "greed", "lust", "envy",
                                                    "gluttony", "wrath", "sloth"};
    static const char *const virtues[SOS_SIN_COUNT] = {
        "humility", "charity", "chastity", "kindness", "temperance", "patience", "diligence"};
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        Expect(strcmp(SosSinsName((SosSin)sin), sins[sin]) == 0, "a sin has its name");
        Expect(strcmp(SosSinsVirtueName((SosSin)sin), virtues[sin]) == 0, "a sin has its virtue");
    }
    Expect(SosSinsName(SOS_SIN_COUNT) == NULL && SosSinsVirtueName((SosSin)-1) == NULL,
           "an unknown sin has no name");
}

static void TestNature(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature first;
    SosSinNature again;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &first), "a nature is born");
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &again), "a nature is born twice");
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        Expect(first.tendency[sin] == again.tendency[sin], "a seed and a being replay a nature");
        Expect(first.tendency[sin] >= 0 && first.tendency[sin] <= SOS_Q16_ONE,
               "a tendency is a chance");
    }
    SosSinNature neighbour;
    Expect(SosSinsNature(SEED, SERIAL + 1u, predisposition, &neighbour), "a neighbour is born");
    int differs = 0;
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        differs |= first.tendency[sin] != neighbour.tendency[sin];
    }
    Expect(differs, "two beings of one seed differ");

    /* A nature never strays more than the variation from its predisposition. */
    FillPredisposition(0, predisposition);
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &first), "a low nature");
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        Expect(first.tendency[sin] >= 0 && first.tendency[sin] <= SOS_SINS_VARIATION,
               "a low predisposition lifts by at most the variation");
    }
    FillPredisposition(SOS_Q16_ONE, predisposition);
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &first), "a high nature");
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        Expect(first.tendency[sin] >= SOS_Q16_ONE - SOS_SINS_VARIATION &&
                   first.tendency[sin] <= SOS_Q16_ONE,
               "a high predisposition drops by at most the variation");
    }
    Expect(!SosSinsNature(SEED, SERIAL, NULL, &first), "no predisposition, no nature");
    Expect(!SosSinsNature(SEED, SERIAL, predisposition, NULL), "no output, no nature");
}

static void ExpectTrend(const SosSinNature *nature, SosSin sin, const SosSinSituation *base,
                        int field, int rising)
{
    SosQ16 previous = rising ? 0 : SOS_Q16_ONE;
    for (int step = 0; step <= 8; ++step)
    {
        SosSinSituation sample = *base;
        SetInput(&sample, field, (SosQ16)((int64_t)SOS_Q16_ONE * step / 8));
        const SosQ16 value = SosSinsTemptation(nature, sin, &sample);
        Expect(value >= 0 && value <= SOS_Q16_ONE, "temptation is always a chance");
        if (rising)
        {
            Expect(value >= previous, "temptation rises with what offers it");
        }
        else
        {
            Expect(value <= previous, "temptation falls with shame and good mood");
        }
        previous = value;
    }
}

static void TestTemptationMonotone(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature nature;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &nature), "a nature for the sweep");
    const SosSinSituation base = {
        .opportunity = SOS_Q16_ONE / 2,
        .need = SOS_Q16_ONE / 2,
        .provocation = SOS_Q16_ONE / 2,
        .rivalAdvantage = SOS_Q16_ONE / 2,
        .watched = SOS_Q16_ONE / 2,
        .mood = SOS_Q16_ONE / 2,
    };
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        for (int field = 0; field < INPUT_COUNT; ++field)
        {
            const int rising = field != INPUT_WATCHED && field != INPUT_MOOD;
            ExpectTrend(&nature, (SosSin)sin, &base, field, rising);
        }
    }
}

static void TestTemptationEdges(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature nature;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &nature), "a nature for the edges");
    const SosSinSituation nothing = {0};
    const SosSinSituation everything = {.opportunity = SOS_Q16_ONE,
                                        .need = SOS_Q16_ONE,
                                        .provocation = SOS_Q16_ONE,
                                        .rivalAdvantage = SOS_Q16_ONE,
                                        .watched = SOS_Q16_ONE,
                                        .mood = SOS_Q16_ONE};
    const SosSinSituation oversize = {.opportunity = INT32_MAX,
                                      .need = INT32_MAX,
                                      .provocation = INT32_MAX,
                                      .rivalAdvantage = INT32_MAX,
                                      .watched = INT32_MAX,
                                      .mood = INT32_MAX};
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        Expect(SosSinsTemptation(&nature, (SosSin)sin, &nothing) == 0,
               "nothing offered tempts nobody");
        Expect(SosSinsTemptation(&nature, (SosSin)sin, &oversize) ==
                   SosSinsTemptation(&nature, (SosSin)sin, &everything),
               "oversize inputs are clamped");
        Expect(SosSinsTemptation(&nature, (SosSin)sin, &everything) > 0,
               "a full opportunity tempts");
    }
    Expect(SosSinsTemptation(NULL, SOS_SIN_PRIDE, &everything) == 0, "no nature, no temptation");
    Expect(SosSinsTemptation(&nature, SOS_SIN_PRIDE, NULL) == 0, "no situation, no temptation");
    Expect(SosSinsTemptation(&nature, SOS_SIN_COUNT, &everything) == 0, "no sin, no temptation");
    Expect(SosSinsTemptation(&nature, (SosSin)-1, &everything) == 0,
           "a negative sin, no temptation");
}

/* Regression: no sin may happen without an opportunity, except wrath, whose
 * provocation is its own occasion. */
static void TestWrathOpportunity(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE, predisposition);
    SosSinNature nature;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &nature), "a nature for wrath");
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        Expect(nature.tendency[sin] > 0, "a full predisposition leans toward every sin");
    }

    /* An injury with no occasion at all: wrath alone answers it. */
    const SosSinSituation provoked = {.provocation = SOS_Q16_ONE};
    Expect(SosSinsTemptation(&nature, SOS_SIN_WRATH, &provoked) > 0,
           "wrath answers provocation without an opportunity");
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        if (sin == SOS_SIN_WRATH)
        {
            continue;
        }
        Expect(SosSinsTemptation(&nature, (SosSin)sin, &provoked) == 0,
               "no other sin happens without an opportunity");
    }

    /* Provocation is what wrath is owed: without an injury it needs an
     * occasion like every other sin. */
    const SosSinSituation nothing = {0};
    Expect(SosSinsTemptation(&nature, SOS_SIN_WRATH, &nothing) == 0, "no injury, no wrath");

    /* A motive without an occasion moves nobody but wrath. */
    const SosSinSituation motivated = {.need = SOS_Q16_ONE,
                                       .provocation = SOS_Q16_ONE,
                                       .rivalAdvantage = SOS_Q16_ONE,
                                       .mood = -SOS_Q16_ONE};
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        if (sin == SOS_SIN_WRATH)
        {
            continue;
        }
        Expect(SosSinsTemptation(&nature, (SosSin)sin, &motivated) == 0,
               "a motive without an opportunity is no temptation");
    }
}

static void TestHabit(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature nature;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &nature), "a nature for habits");
    SosSinRecord record = {0};
    const SosQ16 born = nature.tendency[SOS_SIN_GREED];

    Expect(SosSinsCommit(&nature, &record, SOS_SIN_GREED, SOS_Q16_ONE, 0u, false),
           "a sin is committed");
    Expect(record.committed[SOS_SIN_GREED] == 1u, "a committed sin is counted");
    Expect(nature.tendency[SOS_SIN_GREED] == born + SOS_SINS_HABIT_GAIN,
           "committing strengthens the habit");
    Expect(SosSinsResist(&nature, &record, SOS_SIN_GREED, 0u), "a temptation is resisted");
    Expect(record.resisted[SOS_SIN_GREED] == 1u, "a resisted temptation is counted");
    Expect(nature.tendency[SOS_SIN_GREED] == born + SOS_SINS_HABIT_GAIN - SOS_SINS_HABIT_LOSS,
           "resisting weakens the habit");

    for (int act = 0; act < 1000; ++act)
    {
        Expect(SosSinsCommit(&nature, &record, SOS_SIN_GREED, SOS_Q16_ONE, 0u, false),
               "habit grows");
    }
    Expect(nature.tendency[SOS_SIN_GREED] == SOS_Q16_ONE, "the habit is bounded above");
    for (int act = 0; act < 1000; ++act)
    {
        Expect(SosSinsResist(&nature, &record, SOS_SIN_GREED, 0u), "habit shrinks");
    }
    Expect(nature.tendency[SOS_SIN_GREED] == 0, "the habit is bounded below");

    Expect(!SosSinsCommit(NULL, &record, SOS_SIN_GREED, SOS_Q16_ONE, 0u, false),
           "no nature, no commit");
    Expect(!SosSinsCommit(&nature, NULL, SOS_SIN_GREED, SOS_Q16_ONE, 0u, false),
           "no record, no commit");
    Expect(!SosSinsCommit(&nature, &record, SOS_SIN_COUNT, SOS_Q16_ONE, 0u, false),
           "no sin, no commit");
    Expect(!SosSinsResist(NULL, &record, SOS_SIN_GREED, 0u), "no nature, no resist");
    Expect(!SosSinsResist(&nature, NULL, SOS_SIN_GREED, 0u), "no record, no resist");
}

static void TestGuiltDecay(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature nature;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &nature), "a nature for guilt");
    SosSinRecord record = {0};
    Expect(SosSinsCommit(&nature, &record, SOS_SIN_PRIDE, SOS_Q16_ONE, 0u, false),
           "a sin leaves guilt");
    Expect(SosSinsGuilt(&record, 0u) == SOS_SINS_GUILT_PER_SIN, "guilt is the price of a sin");
    Expect(SosSinsGuilt(&record, SOS_SINS_GUILT_HALF_LIFE) == SOS_SINS_GUILT_PER_SIN / 2,
           "guilt halves in a half-life");
    Expect(SosSinsGuilt(&record, 2 * SOS_SINS_GUILT_HALF_LIFE) == SOS_SINS_GUILT_PER_SIN / 4,
           "guilt halves again");
    Expect(SosSinsGuilt(&record, 64 * SOS_SINS_GUILT_HALF_LIFE) == 0, "guilt fades away");
    Expect(SosSinsGuilt(NULL, 0u) == 0, "no record, no guilt");

    Expect(SosSinsCommit(&nature, &record, SOS_SIN_PRIDE, SOS_Q16_ONE, SOS_SINS_GUILT_HALF_LIFE,
                         false),
           "a later sin");
    Expect(SosSinsGuilt(&record, SOS_SINS_GUILT_HALF_LIFE) ==
               SOS_SINS_GUILT_PER_SIN + SOS_SINS_GUILT_PER_SIN / 2,
           "a later sin adds to the guilt that remains");
}

static void TestNotoriety(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature nature;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &nature), "a nature for notoriety");
    SosSinRecord record = {0};
    Expect(SosSinsCommit(&nature, &record, SOS_SIN_WRATH, SOS_Q16_ONE, 0u, false), "an unseen sin");
    Expect(SosSinsNotoriety(&record, 0u) == 0, "an unseen sin is not notorious");
    Expect(SosSinsCommit(&nature, &record, SOS_SIN_WRATH, SOS_Q16_ONE, 0u, true),
           "a witnessed sin");
    Expect(SosSinsNotoriety(&record, 0u) == SOS_SINS_NOTORIETY_PER_SIN,
           "a witnessed sin leaves notoriety");
    Expect(SosSinsNotoriety(&record, SOS_SINS_NOTORIETY_HALF_LIFE) ==
               SOS_SINS_NOTORIETY_PER_SIN / 2,
           "notoriety fades");
    Expect(SosSinsNotoriety(NULL, 0u) == 0, "no record, no notoriety");
}

static void TestBalance(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature nature;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &nature), "a nature for the balance");
    SosSinRecord record = {0};
    Expect(SosSinsBalance(&record, 0u) == 0, "an untouched ledger is balanced");
    Expect(SosSinsBalance(NULL, 0u) == 0, "no record, no balance");
    Expect(SosSinsCommit(&nature, &record, SOS_SIN_GREED, SOS_Q16_ONE, 0u, true),
           "a witnessed sin");
    Expect(SosSinsBalance(&record, 0u) == -SOS_Q16_ONE, "an unrepented sin is fully sinful");
    for (int act = 0; act < 5; ++act)
    {
        Expect(SosSinsResist(&nature, &record, SOS_SIN_GREED, 0u), "restraint");
    }
    Expect(SosSinsBalance(&record, 0u) > 0, "restraint outweighs a single sin");
}

static void TestHash(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature first;
    SosSinNature second;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &first), "a first ledger");
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &second), "a second ledger");
    SosSinRecord left = {0};
    SosSinRecord right = {0};
    (void)SosSinsCommit(&first, &left, SOS_SIN_ENVY, SOS_Q16_ONE, 0u, true);
    (void)SosSinsCommit(&second, &right, SOS_SIN_ENVY, SOS_Q16_ONE, 0u, true);
    Expect(SosSinsHash(&left, 0u) == SosSinsHash(&right, 0u), "equal ledgers hash equally");
    Expect(SosSinsHash(NULL, 0u) == 0u, "no record, no hash");
    Expect(SosSinsHash(&left, SOS_SINS_GUILT_HALF_LIFE) != SosSinsHash(&left, 0u),
           "guilt decay changes the hash");
    (void)SosSinsCommit(&second, &right, SOS_SIN_WRATH, SOS_Q16_ONE, 0u, false);
    Expect(SosSinsHash(&left, 0u) != SosSinsHash(&right, 0u), "a new sin changes the hash");

    /* Regression: the stamp changes on every write and is part of the state.
     * These two ledgers look identical at tick zero but were written at
     * different moments, and would decay apart later. */
    SosSinRecord soon = {0};
    SosSinRecord later = {0};
    (void)SosSinsCommit(&first, &soon, SOS_SIN_SLOTH, SOS_Q16_ONE / 2, 5u, true);
    (void)SosSinsCommit(&first, &later, SOS_SIN_SLOTH, SOS_Q16_ONE / 2, 50u, true);
    Expect(SosSinsGuilt(&soon, 0u) == SosSinsGuilt(&later, 0u),
           "two ledgers look alike before any time has passed");
    Expect(SosSinsHash(&soon, 0u) != SosSinsHash(&later, 0u),
           "the moment a record was written changes the hash");
}

static void TestNatureHash(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature first;
    SosSinNature copy;
    SosSinNature neighbour;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &first), "a nature to hash");
    copy = first;
    Expect(SosSinsNature(SEED, SERIAL + 1u, predisposition, &neighbour), "a neighbour to hash");
    Expect(SosSinsNatureHash(NULL) == 0u, "no nature, no hash");
    Expect(SosSinsNatureHash(&copy) == SosSinsNatureHash(&first), "equal natures hash equally");
    Expect(SosSinsNatureHash(&first) != SosSinsNatureHash(&neighbour),
           "two beings of one seed hash apart");

    /* The habit changes with every act, so it must change the hash. */
    SosSinRecord record = {0};
    const uint64_t before = SosSinsNatureHash(&first);
    Expect(SosSinsCommit(&first, &record, SOS_SIN_PRIDE, SOS_Q16_ONE, 0u, false),
           "a sin to change the habit");
    Expect(SosSinsNatureHash(&first) != before, "a sin changes the nature hash");
}

/* Fixed points of the module. Every platform and compiler must give exactly
 * these tendencies and this temptation. */
static void TestGolden(void)
{
    SosQ16 predisposition[SOS_SIN_COUNT];
    FillPredisposition(SOS_Q16_ONE / 2, predisposition);
    SosSinNature nature;
    Expect(SosSinsNature(SEED, SERIAL, predisposition, &nature), "the golden nature");
    uint64_t fingerprint = UINT64_C(0);
    for (unsigned sin = 0u; sin < (unsigned)SOS_SIN_COUNT; ++sin)
    {
        fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)nature.tendency[sin]);
    }
    const SosSinSituation situation = {
        .opportunity = SOS_Q16_ONE / 2,
        .need = SOS_Q16_ONE / 4,
        .provocation = SOS_Q16_ONE / 2,
        .rivalAdvantage = SOS_Q16_ONE / 4,
        .watched = SOS_Q16_ONE / 4,
        .mood = SOS_Q16_ONE / 2,
    };
    fingerprint = SosHashCombine(
        fingerprint, (uint64_t)(uint32_t)SosSinsTemptation(&nature, SOS_SIN_ENVY, &situation));
    const uint64_t expected = UINT64_C(0x3EA6C9B44EFA987B);
    if (fingerprint != expected)
    {
        fprintf(stderr, "sins fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the sin module matches the reference");
    }
}

int main(void)
{
    TestNames();
    TestNature();
    TestTemptationMonotone();
    TestTemptationEdges();
    TestWrathOpportunity();
    TestHabit();
    TestGuiltDecay();
    TestNotoriety();
    TestBalance();
    TestHash();
    TestNatureHash();
    TestGolden();
    return 0;
}
