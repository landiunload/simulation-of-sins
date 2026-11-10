#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/status.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define PRESTIGE_HALF_LIFE SOS_STATUS_PRESTIGE_HALF_LIFE
#define DOMINANCE_HALF_LIFE SOS_STATUS_DOMINANCE_HALF_LIFE

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "status test failed: %s\n", message);
        exit(1);
    }
}

static void TestApi(void)
{
    SosStatus *status = SosStatusCreate(SEED);
    Expect(status != NULL, "a status is created");
    SosStanding standing;
    Expect(!SosStatusGet(NULL, 1u, 0u, &standing), "a missing status is rejected");
    Expect(!SosStatusGet(status, 0u, 0u, &standing), "nobody has no standing");
    Expect(!SosStatusGet(status, 1u, 0u, NULL), "a missing output is rejected");
    Expect(SosStatusGet(status, 1u, 0u, &standing) && standing.prestige == SOS_STATUS_BASELINE &&
               standing.dominance == SOS_STATUS_BASELINE,
           "an unknown being stands at the baseline");
    Expect(!SosStatusAward(NULL, 1u, 0u, 1, 0), "a missing status is rejected");
    Expect(!SosStatusAward(status, 0u, 0u, 1, 0), "nobody cannot be awarded");
    SosStatusDestroy(status);
    SosStatusDestroy(NULL);
}

static void TestDecay(void)
{
    SosStatus *status = SosStatusCreate(SEED);
    Expect(status != NULL, "a status is created");
    Expect(SosStatusAward(status, 1u, 0u, 3 * SOS_Q16_ONE / 4, SOS_Q16_ONE / 2),
           "a deed is awarded");
    SosStanding now;
    Expect(SosStatusGet(status, 1u, 0u, &now), "the standing is read");
    const SosQ16 earnedPrestige = (SosQ16)(now.prestige - SOS_STATUS_BASELINE);
    const SosQ16 earnedDominance = (SosQ16)(now.dominance - SOS_STATUS_BASELINE);
    Expect(earnedPrestige > 0 && earnedDominance > 0, "the award is above the baseline");

    SosStanding halfPrestige;
    Expect(SosStatusGet(status, 1u, PRESTIGE_HALF_LIFE, &halfPrestige), "the standing is read");
    Expect(halfPrestige.prestige - SOS_STATUS_BASELINE == earnedPrestige / 2,
           "prestige above the baseline halves in one half-life");

    SosStanding atDominance;
    Expect(SosStatusGet(status, 1u, DOMINANCE_HALF_LIFE, &atDominance), "the standing is read");
    Expect(atDominance.dominance - SOS_STATUS_BASELINE == earnedDominance / 2,
           "dominance above the baseline halves in its own half-life");
    Expect(atDominance.prestige > halfPrestige.prestige, "prestige is remembered longer");

    SosStanding faded;
    Expect(SosStatusGet(status, 1u, 64u * PRESTIGE_HALF_LIFE, &faded), "the standing is read");
    Expect(faded.prestige == SOS_STATUS_BASELINE && faded.dominance == SOS_STATUS_BASELINE,
           "standing returns to the baseline");
    SosStatusDestroy(status);
}

static void TestDiminishing(void)
{
    SosStatus *status = SosStatusCreate(SEED);
    Expect(status != NULL, "a status is created");
    const SosQ16 delta = SOS_Q16_ONE / 4;
    Expect(SosStatusAward(status, 1u, 0u, delta, 0), "a deed is awarded");
    SosStanding first;
    Expect(SosStatusGet(status, 1u, 0u, &first), "the standing is read");
    const SosQ16 firstGain = (SosQ16)(first.prestige - SOS_STATUS_BASELINE);
    Expect(firstGain > 0, "the first award raises the standing");
    Expect(SosStatusAward(status, 1u, 0u, delta, 0), "the deed is awarded again");
    SosStanding second;
    Expect(SosStatusGet(status, 1u, 0u, &second), "the standing is read");
    const SosQ16 secondGain = (SosQ16)(second.prestige - first.prestige);
    Expect(secondGain < firstGain, "the same deed is worth less near the top");
    Expect(secondGain > 0, "it is still worth something");

    for (int step = 0; step < 100; ++step)
    {
        Expect(SosStatusAward(status, 2u, 0u, SOS_Q16_ONE, SOS_Q16_ONE), "a great deed is awarded");
    }
    SosStanding capped;
    Expect(SosStatusGet(status, 2u, 0u, &capped), "the standing is read");
    Expect(capped.prestige <= SOS_Q16_ONE && capped.prestige > 0 && capped.dominance > 0 &&
               capped.dominance <= SOS_Q16_ONE,
           "standing stays in range however much is earned");

    Expect(SosStatusAward(status, 3u, 0u, 0, 0), "a zero award is accepted");
    Expect(SosStatusAward(status, 3u, 0u, -SOS_Q16_ONE, -SOS_Q16_ONE), "shame is awarded");
    SosStanding shamed;
    Expect(SosStatusGet(status, 3u, 0u, &shamed), "the standing is read");
    Expect(shamed.prestige == 0 && shamed.dominance == 0, "shame floors at zero");
    SosStanding healed;
    Expect(SosStatusGet(status, 3u, PRESTIGE_HALF_LIFE, &healed), "the standing is read");
    Expect(healed.prestige == SOS_STATUS_BASELINE / 2, "a shamed being climbs back");
    SosStatusDestroy(status);
}

static void TestRank(void)
{
    SosStatus *status = SosStatusCreate(SEED);
    Expect(status != NULL, "a status is created");
    /* Serial 5 earns only prestige and serial 2 only dominance: respect is
     * weighted above fear, so 5 leads although 2 is the lower serial. */
    for (int step = 0; step < 100; ++step)
    {
        Expect(SosStatusAward(status, 5u, 0u, SOS_Q16_ONE, 0), "prestige is awarded");
        Expect(SosStatusAward(status, 2u, 0u, 0, SOS_Q16_ONE), "dominance is awarded");
    }
    const uint64_t pair[] = {2u, 5u};
    uint64_t order[2] = {0u, 0u};
    Expect(SosStatusRank(status, 0u, pair, 2u, order) == 2u, "both beings are ordered");
    Expect(order[0] == 5u && order[1] == 2u, "prestige outweighs dominance");

    /* Equal standing goes to the lower serial. */
    Expect(SosStatusAward(status, 3u, 0u, SOS_Q16_ONE / 2, 0), "prestige is awarded");
    Expect(SosStatusAward(status, 7u, 0u, SOS_Q16_ONE / 2, 0), "prestige is awarded");
    const uint64_t tied[] = {7u, 3u};
    uint64_t tiedOrder[2] = {0u, 0u};
    Expect(SosStatusRank(status, 0u, tied, 2u, tiedOrder) == 2u, "both are ordered");
    Expect(tiedOrder[0] == 3u && tiedOrder[1] == 7u, "a tie goes to the lower serial");

    /* More deeds always rank above fewer. */
    for (uint32_t being = 10u; being <= 30u; being += 10u)
    {
        for (uint32_t deed = 0u; deed < being / 10u; ++deed)
        {
            Expect(SosStatusAward(status, being, 0u, SOS_Q16_ONE / 8, 0), "prestige is awarded");
        }
    }
    const uint64_t group[] = {10u, 20u, 30u};
    uint64_t groupOrder[3] = {0u, 0u, 0u};
    Expect(SosStatusRank(status, 0u, group, 3u, groupOrder) == 3u, "the group is ordered");
    Expect(groupOrder[0] == 30u && groupOrder[1] == 20u && groupOrder[2] == 10u,
           "more standing ranks first");

    Expect(SosStatusRank(status, 0u, pair, 0u, order) == 0u, "an empty group orders nothing");
    Expect(SosStatusRank(NULL, 0u, pair, 2u, order) == 0u, "a missing status orders nothing");
    Expect(SosStatusRank(status, 0u, NULL, 2u, order) == 0u, "a missing group orders nothing");
    Expect(SosStatusRank(status, 0u, pair, 2u, NULL) == 0u, "a missing output orders nothing");
    SosStatusDestroy(status);
}

static void TestInherit(void)
{
    SosStatus *status = SosStatusCreate(SEED);
    Expect(status != NULL, "a status is created");
    Expect(SosStatusAward(status, 10u, 0u, SOS_Q16_ONE / 2, 0), "the mother earns prestige");
    Expect(SosStatusAward(status, 11u, 0u, SOS_Q16_ONE / 4, 0), "the father earns prestige");
    SosStanding mother;
    SosStanding father;
    Expect(SosStatusGet(status, 10u, 0u, &mother), "the mother is read");
    Expect(SosStatusGet(status, 11u, 0u, &father), "the father is read");
    const SosQ16 average = (SosQ16)(((int64_t)mother.prestige + father.prestige) / 2);
    const SosQ16 expected =
        (SosQ16)(SOS_STATUS_BASELINE + SosQ16Mul(average, SOS_STATUS_INHERIT_SHARE));
    Expect(SosStatusInherit(status, 1u, 10u, 11u, 0u), "a child inherits");
    SosStanding child;
    Expect(SosStatusGet(status, 1u, 0u, &child), "the child is read");
    Expect(child.prestige == expected, "a child gets the share of its parents' prestige");
    Expect(child.dominance == SOS_STATUS_BASELINE, "dominance is not inherited");

    Expect(SosStatusInherit(status, 2u, 10u, 0u, 0u), "a child inherits from one parent");
    SosStanding half;
    Expect(SosStatusGet(status, 2u, 0u, &half), "the child is read");
    Expect(half.prestige ==
               SOS_STATUS_BASELINE + SosQ16Mul(mother.prestige, SOS_STATUS_INHERIT_SHARE),
           "a lone parent passes its share");

    Expect(!SosStatusInherit(status, 0u, 10u, 11u, 0u), "nobody cannot inherit");
    Expect(!SosStatusInherit(status, 3u, 0u, 0u, 0u), "a child without parents inherits nothing");
    Expect(!SosStatusInherit(NULL, 3u, 10u, 11u, 0u), "a missing status inherits nothing");
    SosStatusDestroy(status);
}

static void TestForgetAndPrune(void)
{
    SosStatus *status = SosStatusCreate(SEED);
    Expect(status != NULL, "a status is created");
    const uint64_t empty = SosStatusHash(status, 0u);
    Expect(SosStatusAward(status, 1u, 0u, SOS_Q16_ONE / 2, SOS_Q16_ONE / 4), "a deed is awarded");
    Expect(SosStatusHash(status, 0u) != empty, "a record changes the hash");
    Expect(SosStatusHash(status, 64u * PRESTIGE_HALF_LIFE) != SosStatusHash(status, 0u),
           "the hash follows decay");
    Expect(SosStatusForget(status, 1u), "a record is forgotten");
    Expect(!SosStatusForget(status, 1u), "forgetting twice changes nothing");
    Expect(!SosStatusForget(status, 0u), "nobody is never forgotten");
    Expect(!SosStatusForget(NULL, 1u), "a missing status forgets nothing");
    Expect(SosStatusHash(status, 0u) == empty, "a forgotten record leaves no trace");

    Expect(SosStatusAward(status, 4u, 0u, SOS_Q16_ONE / 2, SOS_Q16_ONE / 2), "a deed is awarded");
    Expect(SosStatusPrune(status, 0u) == 0u, "fresh standing is not pruned");
    const uint64_t faded = SosStatusHash(status, 64u * PRESTIGE_HALF_LIFE);
    Expect(faded == empty, "standing decayed to the baseline counts as none");
    Expect(SosStatusPrune(status, 64u * PRESTIGE_HALF_LIFE) == 1u, "decayed standing is pruned");
    Expect(SosStatusHash(status, 64u * PRESTIGE_HALF_LIFE) == faded, "pruning does not change it");
    Expect(SosStatusPrune(NULL, 0u) == 0u, "a missing status prunes nothing");
    SosStatusDestroy(status);
}

static void TestHashOrder(void)
{
    SosStatus *first = SosStatusCreate(SEED);
    SosStatus *second = SosStatusCreate(SEED);
    Expect(first != NULL && second != NULL, "statuses are created");
    Expect(SosStatusAward(first, 1u, 0u, SOS_Q16_ONE / 2, 0), "a deed is awarded");
    Expect(SosStatusAward(first, 2u, 0u, 0, SOS_Q16_ONE / 3), "a deed is awarded");
    Expect(SosStatusAward(first, 3u, 0u, SOS_Q16_ONE / 4, SOS_Q16_ONE / 4), "a deed is awarded");
    Expect(SosStatusAward(second, 3u, 0u, SOS_Q16_ONE / 4, SOS_Q16_ONE / 4), "a deed is awarded");
    Expect(SosStatusAward(second, 2u, 0u, 0, SOS_Q16_ONE / 3), "a deed is awarded");
    Expect(SosStatusAward(second, 1u, 0u, SOS_Q16_ONE / 2, 0), "a deed is awarded");
    Expect(SosStatusHash(first, 0u) == SosStatusHash(second, 0u),
           "the order of awards does not change the hash");
    SosStatusDestroy(first);
    SosStatusDestroy(second);
}

/* Fixed points of the standing layer. Every platform and compiler must fold
 * exactly this value. */
static void TestGolden(void)
{
    SosStatus *status = SosStatusCreate(SEED);
    Expect(status != NULL, "a status is created");
    for (uint32_t being = 1u; being <= 16u; ++being)
    {
        Expect(SosStatusAward(status, being, 0u, (SosQ16)(being * (SOS_Q16_ONE / 32)),
                              SOS_Q16_ONE / 8),
               "a deed is awarded");
    }
    Expect(SosStatusInherit(status, 100u, 1u, 2u, 0u), "a child inherits");
    const uint64_t hash = SosStatusHash(status, SOS_TICKS_PER_YEAR / 2);
    const uint64_t expected = UINT64_C(0x521D71762586E1C6);
    if (hash != expected)
    {
        fprintf(stderr, "status fingerprint: 0x%016" PRIX64 "\n", hash);
        Expect(0, "the standing matches the reference");
    }
    SosStatusDestroy(status);
}

int main(void)
{
    TestApi();
    TestDecay();
    TestDiminishing();
    TestRank();
    TestInherit();
    TestForgetAndPrune();
    TestHashOrder();
    TestGolden();
    return 0;
}
