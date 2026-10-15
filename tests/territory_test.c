#include "sim/calendar.h"
#include "sim/random.h"
#include "sim/territory.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "territory test failed: %s\n", message);
        exit(1);
    }
}

/* A point on the ground plane, in metres from the origin. */
static SosPosition At(int64_t xMetres, int64_t yMetres)
{
    return (SosPosition){SOS_METRES(xMetres), SOS_METRES(yMetres), 0};
}

static void TestMarkAndOwner(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const uint64_t tick = 1000u;
    uint64_t group = 0u;
    SosQ16 strength = 0;

    Expect(SosTerritoryMark(territory, 7u, At(10, -10), SOS_METRES(1), tick, SOS_Q16_ONE) == 1u,
           "a small mark touches one cell");
    Expect(SosTerritoryOwner(territory, At(10, -10), tick, &group, &strength) && group == 7u &&
               strength == SOS_Q16_ONE,
           "the marker owns the cell it marked");
    Expect(!SosTerritoryOwner(territory, At(200, 200), tick, &group, &strength),
           "an unmarked cell has no owner");

    /* The same cell is claimed by a rival with half the strength. */
    Expect(SosTerritoryMark(territory, 9u, At(10, -10), SOS_METRES(1), tick, SOS_Q16_ONE / 2) == 1u,
           "a second group claims the same cell");
    Expect(SosTerritoryOwner(territory, At(10, -10), tick, &group, &strength) && group == 7u,
           "the stronger claim still owns the cell");

    /* A radius of 128 m reaches five cells per side around the origin. */
    Expect(SosTerritoryMark(territory, 3u, At(0, 0), SOS_METRES(128), tick, SOS_Q16_ONE) == 25u,
           "a 128 m mark covers a five by five block");
    Expect(SosTerritoryOwner(territory, At(120, -120), tick, &group, &strength) && group == 3u,
           "the far corner of the block is owned");

    SosTerritoryDestroy(territory);
}

static void TestCrowded(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const uint64_t tick = 500u;
    const SosPosition here = At(0, 0);
    for (uint64_t group = 1u; group <= SOS_TERRITORY_CLAIMS; ++group)
    {
        Expect(SosTerritoryMark(territory, group, here, 0, tick, SOS_Q16_ONE) == 1u,
               "each of the four first claims is kept");
    }
    uint64_t group = 0u;
    SosQ16 strength = 0;
    Expect(SosTerritoryOwner(territory, here, tick, &group, &strength) && group == 1u &&
               strength == SOS_Q16_ONE,
           "equal claims tie to the lower group id");
    /* The cell is full and the newcomer is weaker than every claim. */
    Expect(SosTerritoryMark(territory, 99u, here, 0, tick, SOS_Q16_ONE / 2) == 0u,
           "a weaker newcomer cannot enter a full cell");
    /* A stronger newcomer replaces the weakest claim. */
    Expect(SosTerritoryMark(territory, 50u, here, 0, tick, 2 * SOS_Q16_ONE) == 1u,
           "a stronger newcomer takes a full cell");
    Expect(SosTerritoryOwner(territory, here, tick, &group, &strength) && group == 50u &&
               strength == 2 * SOS_Q16_ONE,
           "the newcomer owns the cell");
    SosTerritoryDestroy(territory);
}

static void TestDecay(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const SosPosition here = At(0, 0);
    Expect(SosTerritoryMark(territory, 5u, here, 0, 0u, SOS_Q16_ONE) == 1u, "marking at tick zero");
    uint64_t group = 0u;
    SosQ16 strength = 0;
    Expect(SosTerritoryOwner(territory, here, 0u, &group, &strength) && strength == SOS_Q16_ONE,
           "a fresh claim has full strength");
    Expect(SosTerritoryOwner(territory, here, SOS_TERRITORY_HALF_LIFE, &group, &strength) &&
               strength == SOS_Q16_ONE / 2,
           "a claim halves after one half-life");
    Expect(SosTerritoryOwner(territory, here, 3u * SOS_TERRITORY_HALF_LIFE, &group, &strength) &&
               strength == SOS_Q16_ONE / 8,
           "a claim is an eighth after three half-lives");
    Expect(!SosTerritoryOwner(territory, here, 5u * SOS_TERRITORY_HALF_LIFE, &group, &strength),
           "a claim below the owner threshold owns nothing");
    Expect(!SosTerritoryOwner(territory, here, 100u * SOS_TERRITORY_HALF_LIFE, &group, &strength),
           "a claim decayed to nothing owns nothing");

    /* Renewing at a later tick decays first and then adds, so presence keeps
     * a claim alive where nobody renewing it would lose it. */
    Expect(SosTerritoryMark(territory, 5u, here, 0, 3u * SOS_TERRITORY_HALF_LIFE, SOS_Q16_ONE) ==
               1u,
           "the group renews its claim");
    Expect(SosTerritoryOwner(territory, here, 3u * SOS_TERRITORY_HALF_LIFE, &group, &strength) &&
               strength == SOS_Q16_ONE + SOS_Q16_ONE / 8,
           "renewing decays the old strength and adds the new");
    SosTerritoryDestroy(territory);
}

static void TestContest(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const uint64_t tick = 20u;
    Expect(!SosTerritoryContested(territory, At(0, 0), tick), "unclaimed land is not contested");
    Expect(SosTerritoryMark(territory, 1u, At(0, 0), 0, tick, SOS_Q16_ONE) == 1u,
           "a lone claim is placed");
    Expect(!SosTerritoryContested(territory, At(0, 0), tick), "one claim is not a contest");
    Expect(SosTerritoryMark(territory, 2u, At(0, 0), 0, tick, SOS_Q16_ONE / 4) == 1u,
           "a weak rival joins");
    Expect(!SosTerritoryContested(territory, At(0, 0), tick),
           "a claim well below half is not a contest");
    Expect(SosTerritoryMark(territory, 2u, At(0, 0), 0, tick, SOS_Q16_ONE / 2) == 1u,
           "the rival closes the gap");
    Expect(SosTerritoryContested(territory, At(0, 0), tick), "a rival within half is a contest");
    SosTerritoryDestroy(territory);
}

static void TestAbandon(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const uint64_t tick = 7u;
    Expect(SosTerritoryMark(territory, 4u, At(0, 0), SOS_METRES(64), tick, SOS_Q16_ONE) == 9u,
           "a 64 m mark covers a three by three block");
    Expect(SosTerritoryMark(territory, 5u, At(0, 0), 0, tick, SOS_Q16_ONE / 2) == 1u,
           "a rival claims the centre");
    Expect(SosTerritoryAbandon(territory, 4u) == 9u, "abandoning removes every claim of the group");
    uint64_t group = 0u;
    SosQ16 strength = 0;
    Expect(SosTerritoryOwner(territory, At(0, 0), tick, &group, &strength) && group == 5u,
           "the rival now owns the centre");
    Expect(!SosTerritoryOwner(territory, At(64, 0), tick, &group, &strength),
           "the rest of the land is free");
    Expect(SosTerritoryAbandon(territory, 4u) == 0u, "abandoning again removes nothing");
    Expect(SosTerritoryAbandon(territory, 5u) == 1u, "the rival abandons its last claim");
    Expect(!SosTerritoryOwner(territory, At(0, 0), tick, &group, &strength), "the land is empty");
    SosTerritoryDestroy(territory);
}

static void TestPrune(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const SosPosition here = At(300, -300);
    Expect(SosTerritoryMark(territory, 4u, here, 0, 0u, SOS_Q16_ONE) == 1u, "a claim is placed");
    Expect(SosTerritoryPrune(territory, 0u) == 0u, "a fresh claim is not pruned");
    const uint64_t far = 100u * SOS_TERRITORY_HALF_LIFE;
    /* At `far` the claim has already decayed to nothing, so the hash is that
     * of empty land and pruning changes nothing about it. */
    const uint64_t before = SosTerritoryHash(territory, far);
    Expect(SosTerritoryPrune(territory, far) == 1u, "a decayed claim is pruned");
    Expect(SosTerritoryHash(territory, far) == before, "pruning does not change the hash");
    Expect(SosTerritoryPrune(territory, far) == 0u, "pruning an empty territory removes nothing");
    uint64_t group = 0u;
    SosQ16 strength = 0;
    Expect(!SosTerritoryOwner(territory, here, 0u, &group, &strength), "pruned land has no owner");
    SosTerritoryDestroy(territory);
}

/* Abandon empties a slot without moving the claims behind it, so a group that
 * renews after a middle claim went must find its own claim rather than take
 * the hole and hold the cell twice. */
static void TestUniqueAfterAbandon(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const SosPosition here = At(10, -20);
    const uint64_t tick = 4u;
    for (uint64_t group = 1u; group <= SOS_TERRITORY_CLAIMS; ++group)
    {
        Expect(SosTerritoryMark(territory, group, here, 0, tick, SOS_Q16_ONE) == 1u,
               "each group claims the full cell");
    }
    Expect(SosTerritoryAbandon(territory, 3u) == 1u, "the middle claim is abandoned");
    Expect(SosTerritoryMark(territory, 4u, here, 0, tick, SOS_Q16_ONE) == 1u,
           "the group behind the hole renews");
    Expect(SosTerritoryAbandon(territory, 4u) == 1u,
           "the renewed group holds exactly one claim, not two");
    SosTerritoryDestroy(territory);
}

/* Prune removes a decayed claim and leaves its slot empty; the surviving
 * group must renew its own claim instead of filling that hole with a second
 * claim of itself, which SosTerritoryContested would read as a contest. */
static void TestUniqueAfterPrune(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const SosPosition here = At(-40, 30);
    Expect(SosTerritoryMark(territory, 1u, here, 0, 0u, SOS_Q16_ONE) == 1u,
           "an old claim is placed");
    const uint64_t later = 100u * SOS_TERRITORY_HALF_LIFE;
    Expect(SosTerritoryMark(territory, 2u, here, 0, later, SOS_Q16_ONE) == 1u,
           "a fresh claim joins before the old one fades");
    Expect(SosTerritoryPrune(territory, later) == 1u, "only the decayed claim is pruned");
    Expect(SosTerritoryMark(territory, 2u, here, 0, later, SOS_Q16_ONE) == 1u,
           "the survivor renews behind the hole");
    Expect(SosTerritoryAbandon(territory, 2u) == 1u,
           "the survivor holds exactly one claim, not two");
    Expect(!SosTerritoryContested(territory, here, later), "one group cannot contest itself");
    SosTerritoryDestroy(territory);
}

static void TestCellMath(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const uint64_t tick = 3u;
    /* Just below the -64 m boundary: this cell reaches down to -128 m. */
    const SosPosition marked = {-SOS_METRES(64) - 1, -SOS_METRES(64) - 1, 0};
    Expect(SosTerritoryMark(territory, 8u, marked, 0, tick, SOS_Q16_ONE) == 1u,
           "a cell below the origin is marked");
    uint64_t group = 0u;
    Expect(SosTerritoryOwner(territory, marked, tick, &group, NULL) && group == 8u,
           "the marked point is owned");
    Expect(SosTerritoryOwner(territory, (SosPosition){-SOS_METRES(65), -SOS_METRES(65), 0}, tick,
                             &group, NULL) &&
               group == 8u,
           "another point of the same cell is owned");
    Expect(!SosTerritoryOwner(territory, (SosPosition){-SOS_METRES(64), -SOS_METRES(64), 0}, tick,
                              &group, NULL),
           "the neighbouring cell across the boundary is not owned");
    SosTerritoryDestroy(territory);
}

/* The same marks in a different order must give the same world: the table's
 * slot order may differ but the hash and every query must not. */
static uint64_t WorldFingerprint(int order)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    if (order == 0)
    {
        (void)SosTerritoryMark(territory, 1u, At(0, 0), SOS_METRES(200), 0u, SOS_Q16_ONE);
        (void)SosTerritoryMark(territory, 2u, At(50, 50), SOS_METRES(120), 1000u, SOS_Q16_ONE / 2);
    }
    else
    {
        (void)SosTerritoryMark(territory, 2u, At(50, 50), SOS_METRES(120), 1000u, SOS_Q16_ONE / 2);
        (void)SosTerritoryMark(territory, 1u, At(0, 0), SOS_METRES(200), 0u, SOS_Q16_ONE);
    }
    uint64_t fingerprint = SosTerritoryHash(territory, 2000u);
    for (int64_t cell = -3; cell <= 3; ++cell)
    {
        const SosPosition at = At(cell * 64, cell * 64);
        uint64_t group = 0u;
        SosQ16 strength = 0;
        if (SosTerritoryOwner(territory, at, 2000u, &group, &strength))
        {
            fingerprint = SosHashCombine(fingerprint, group);
            fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)strength);
        }
        fingerprint =
            SosHashCombine(fingerprint, SosTerritoryContested(territory, at, 2000u) ? 1u : 0u);
    }
    SosTerritoryDestroy(territory);
    return fingerprint;
}

static void TestDeterminism(void)
{
    const uint64_t first = WorldFingerprint(0);
    const uint64_t second = WorldFingerprint(1);
    Expect(first != 0u, "the marked world is not empty");
    Expect(first == second, "the order of marks does not change the world");
}

static void TestRobustness(void)
{
    const SosPosition here = At(0, 0);
    Expect(SosTerritoryMark(NULL, 1u, here, 0, 0u, SOS_Q16_ONE) == 0u,
           "a null territory cannot be marked");
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    Expect(SosTerritoryMark(territory, 0u, here, 0, 0u, SOS_Q16_ONE) == 0u,
           "group zero is rejected");
    Expect(SosTerritoryMark(territory, 1u, here, -1, 0u, SOS_Q16_ONE) == 0u,
           "a negative radius is rejected");
    Expect(SosTerritoryMark(territory, 1u, here, 0, 0u, 0) == 0u, "a zero amount is rejected");
    Expect(SosTerritoryMark(territory, 1u, here, 0, 0u, -SOS_Q16_ONE) == 0u,
           "a negative amount is rejected");
    Expect(SosTerritoryMark(territory, 1u, here, INT64_MAX / 2, 0u, SOS_Q16_ONE) == 0u,
           "an oversized radius is rejected");
    Expect(SosTerritoryMark(territory, 1u, (SosPosition){INT64_MAX, INT64_MAX, 0}, INT64_MAX, 0u,
                            SOS_Q16_ONE) == 0u,
           "a radius off the world's edge is rejected");
    uint64_t group = 0u;
    SosQ16 strength = 0;
    Expect(!SosTerritoryOwner(NULL, here, 0u, &group, &strength), "a null territory has no owner");
    Expect(!SosTerritoryContested(NULL, here, 0u), "a null territory has no contest");
    Expect(SosTerritoryAbandon(NULL, 1u) == 0u, "a null territory cannot abandon");
    Expect(SosTerritoryPrune(NULL, 0u) == 0u, "a null territory cannot prune");
    Expect(SosTerritoryHash(NULL, 0u) == 0u, "a null territory hashes to zero");
    Expect(SosTerritoryAbandon(territory, 0u) == 0u, "group zero abandons nothing");
    Expect(!SosTerritoryOwner(territory, here, 0u, NULL, NULL), "the outputs are optional");

    /* Marking the same group again saturates instead of overflowing. */
    Expect(SosTerritoryMark(territory, 6u, here, 0, 0u, SOS_Q16_ONE) == 1u, "a claim is placed");
    Expect(SosTerritoryMark(territory, 6u, here, 0, 0u, 4 * SOS_Q16_ONE) == 1u,
           "the claim is strengthened");
    Expect(SosTerritoryOwner(territory, here, 0u, &group, &strength) &&
               strength == SOS_TERRITORY_MAX_STRENGTH,
           "strength saturates at the maximum");
    SosTerritoryDestroy(territory);
    SosTerritoryDestroy(NULL);
}

/* A fixed world of marks. Every platform and compiler must fold it to exactly
 * the value pinned below. */
static uint64_t GoldenFingerprint(void)
{
    SosTerritory *territory = SosTerritoryCreate();
    Expect(territory != NULL, "territory is created");
    const uint64_t base = 10u * SOS_TICKS_PER_DAY;
    const uint64_t seen = base + 6u * SOS_TICKS_PER_DAY;
    (void)SosTerritoryMark(territory, 11u, At(0, 0), SOS_METRES(150), base, SOS_Q16_ONE);
    (void)SosTerritoryMark(territory, 22u, At(70, 30), SOS_METRES(90), base, SOS_Q16_ONE / 2);
    (void)SosTerritoryMark(territory, 33u, At(-120, 60), SOS_METRES(40),
                           base + 2u * SOS_TICKS_PER_DAY, SOS_Q16_ONE / 4);
    (void)SosTerritoryMark(territory, 11u, At(-130, 70), SOS_METRES(30),
                           base + 4u * SOS_TICKS_PER_DAY, SOS_Q16_ONE);
    uint64_t fingerprint = SosTerritoryHash(territory, seen);
    for (int64_t gx = -3; gx <= 3; ++gx)
    {
        for (int64_t gy = -3; gy <= 3; ++gy)
        {
            const SosPosition at = At(gx * 40, gy * 40);
            uint64_t group = 0u;
            SosQ16 strength = 0;
            if (SosTerritoryOwner(territory, at, seen, &group, &strength))
            {
                fingerprint = SosHashCombine(fingerprint, group);
                fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)strength);
            }
            fingerprint =
                SosHashCombine(fingerprint, SosTerritoryContested(territory, at, seen) ? 1u : 0u);
        }
    }
    SosTerritoryDestroy(territory);
    return fingerprint;
}

static void TestGoldenValues(void)
{
    const uint64_t expected = UINT64_C(0x055327C192571A6A);
    const uint64_t fingerprint = GoldenFingerprint();
    if (fingerprint != expected)
    {
        fprintf(stderr, "territory fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the territory matches the reference");
    }
}

int main(void)
{
    TestMarkAndOwner();
    TestCrowded();
    TestDecay();
    TestContest();
    TestAbandon();
    TestPrune();
    TestUniqueAfterAbandon();
    TestUniqueAfterPrune();
    TestCellMath();
    TestDeterminism();
    TestRobustness();
    TestGoldenValues();
    return 0;
}
