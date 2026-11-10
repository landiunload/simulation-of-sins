#include "sim/calendar.h"
#include "sim/fixed.h"
#include "sim/random.h"
#include "sim/relations.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "relations test failed: %s\n", message);
        exit(1);
    }
}

static bool Contains(const uint64_t *values, uint32_t count, uint64_t value)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (values[index] == value)
        {
            return true;
        }
    }
    return false;
}

static void TestStranger(const SosRelations *relations)
{
    SosRelation relation = {1, 1, 1};
    Expect(!SosRelationsGet(relations, 1u, 2u, 0u, &relation), "strangers have no relation");
    Expect(relation.affinity == 0 && relation.trust == 0 && relation.familiarity == 0,
           "a stranger reads as neutral");
    Expect(!SosRelationsGet(NULL, 1u, 2u, 0u, &relation), "a NULL graph has no relation");
    Expect(!SosRelationsGet(relations, 0u, 2u, 0u, &relation), "nobody has no relation");
    Expect(!SosRelationsGet(relations, 1u, 0u, 0u, &relation), "nobody is no relation");
    Expect(!SosRelationsGet(relations, 1u, 2u, 0u, NULL), "a NULL out is rejected");
}

static void TestAdjustAndClamp(void)
{
    SosRelations *relations = SosRelationsCreate();
    Expect(relations != NULL, "relations are created");
    SosRelation relation;
    Expect(SosRelationsAdjust(relations, 1u, 2u, 0u, SOS_Q16_ONE, SOS_Q16_ONE),
           "an interaction is recorded");
    Expect(SosRelationsGet(relations, 1u, 2u, 0u, &relation) && relation.affinity == SOS_Q16_ONE &&
               relation.trust == SOS_Q16_ONE,
           "the deltas are stored");
    Expect(relation.familiarity == SOS_RELATIONS_FAMILIARITY_STEP,
           "an interaction raises familiarity");
    Expect(SosRelationsAdjust(relations, 1u, 2u, 0u, SOS_Q16_ONE, 0), "duplicate adjust works");
    Expect(SosRelationsGet(relations, 1u, 2u, 0u, &relation) && relation.affinity == SOS_Q16_ONE,
           "affinity is clamped high");
    Expect(relation.familiarity == 2 * SOS_RELATIONS_FAMILIARITY_STEP,
           "familiarity rises with each interaction");
    Expect(SosRelationsAdjust(relations, 1u, 2u, 0u, INT32_MIN, INT32_MAX),
           "extreme deltas are accepted");
    Expect(SosRelationsGet(relations, 1u, 2u, 0u, &relation) && relation.affinity == -SOS_Q16_ONE &&
               relation.trust == SOS_Q16_ONE,
           "extreme deltas are clamped");
    for (int repeat = 0; repeat < 32; ++repeat)
    {
        Expect(SosRelationsAdjust(relations, 1u, 2u, 0u, 0, 0), "familiarity saturates");
    }
    Expect(SosRelationsGet(relations, 1u, 2u, 0u, &relation) && relation.familiarity == SOS_Q16_ONE,
           "familiarity is clamped high");

    Expect(!SosRelationsAdjust(relations, 0u, 2u, 0u, 0, 0), "nobody cannot feel");
    Expect(!SosRelationsAdjust(relations, 1u, 1u, 0u, 0, 0), "a being cannot feel for itself");
    Expect(!SosRelationsAdjust(NULL, 1u, 2u, 0u, 0, 0), "a NULL graph rejects an interaction");
    SosRelationsDestroy(relations);
}

static void TestDecay(void)
{
    SosRelations *relations = SosRelationsCreate();
    Expect(relations != NULL, "relations are created");
    Expect(SosRelationsAdjust(relations, 1u, 2u, 0u, SOS_Q16_ONE, SOS_Q16_ONE),
           "an interaction is recorded");
    SosRelation relation;
    Expect(SosRelationsGet(relations, 1u, 2u, 0u, &relation) && relation.affinity == SOS_Q16_ONE &&
               relation.trust == SOS_Q16_ONE,
           "the feeling is fresh at the moment of contact");
    Expect(SosRelationsGet(relations, 1u, 2u, SOS_TICKS_PER_YEAR, &relation) &&
               relation.affinity == SOS_Q16_ONE / 2 && relation.trust == SOS_Q16_ONE / 2,
           "a feeling halves in a year");
    Expect(SosRelationsGet(relations, 1u, 2u, 2 * (uint64_t)SOS_TICKS_PER_YEAR, &relation) &&
               relation.affinity == SOS_Q16_ONE / 4,
           "a feeling quarters in two years");
    const uint64_t familiarity = (uint64_t)SOS_RELATIONS_FAMILIARITY_STEP;
    Expect(SosRelationsGet(relations, 1u, 2u, (uint64_t)SOS_TICKS_PER_YEAR, &relation) &&
               relation.familiarity == (SosQ16)SosDecay((int64_t)familiarity,
                                                        (uint64_t)SOS_TICKS_PER_YEAR,
                                                        SOS_RELATIONS_FAMILIARITY_HALF_LIFE),
           "familiarity fades on its own timescale");
    Expect(SosRelationsGet(relations, 1u, 2u, 100 * (uint64_t)SOS_TICKS_PER_YEAR, &relation) &&
               relation.affinity == 0 && relation.trust == 0 && relation.familiarity == 0,
           "everything fades to neutral");
    Expect(SosRelationsGet(relations, 1u, 2u, UINT64_MAX, &relation) && relation.affinity == 0,
           "an extreme tick fades to neutral");
    /* Time before the last contact cannot add decay. */
    Expect(SosRelationsAdjust(relations, 1u, 2u, 10 * (uint64_t)SOS_TICKS_PER_YEAR, SOS_Q16_ONE, 0),
           "the contact is renewed in later years");
    Expect(SosRelationsGet(relations, 1u, 2u, 5 * (uint64_t)SOS_TICKS_PER_YEAR, &relation) &&
               relation.affinity == SOS_Q16_ONE,
           "a tick before the last contact adds no decay");
    SosRelationsDestroy(relations);
}

static void TestMutual(void)
{
    SosRelations *relations = SosRelationsCreate();
    Expect(relations != NULL, "relations are created");
    Expect(SosRelationsAdjust(relations, 1u, 2u, 0u, SOS_Q16_ONE / 2, 0), "one feels warmth");
    Expect(SosRelationsAdjust(relations, 2u, 1u, 0u, -SOS_Q16_ONE / 4, 0), "the other feels cold");
    const SosQ16 mutual = SosRelationsMutual(relations, 1u, 2u, 0u);
    Expect(mutual == SOS_Q16_ONE / 8, "mutual feeling is their average");
    Expect(mutual == SosRelationsMutual(relations, 2u, 1u, 0u), "mutual feeling is symmetric");
    Expect(SosRelationsMutual(relations, 1u, 3u, 0u) == 0, "a stranger is neutral");
    Expect(SosRelationsMutual(relations, 1u, 1u, 0u) == 0,
           "a being feels nothing mutual for itself");
    Expect(SosRelationsMutual(NULL, 1u, 2u, 0u) == 0, "a NULL graph has no mutual feeling");
    SosRelationsDestroy(relations);
}

static void TestListAndForget(void)
{
    SosRelations *relations = SosRelationsCreate();
    Expect(relations != NULL, "relations are created");
    const uint64_t others[3] = {7u, 8u, 9u};
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        Expect(SosRelationsAdjust(relations, 5u, others[index], 0u, SOS_Q16_ONE / 4, 0),
               "a relation is recorded");
    }
    Expect(SosRelationsAdjust(relations, 7u, 5u, 0u, SOS_Q16_ONE, 0), "the other feels back");
    SosRelationEntry entries[4];
    const uint32_t count = SosRelationsOf(relations, 5u, 0u, entries, 4u);
    Expect(count == 3u, "a being has its own degree");
    for (uint32_t index = 0u; index < count; ++index)
    {
        Expect(Contains(others, 3u, entries[index].other), "every relation is enumerated");
        Expect(entries[index].relation.affinity == SOS_Q16_ONE / 4, "an entry carries its feeling");
    }
    Expect(SosRelationsOf(relations, 5u, 0u, entries, 1u) == 3u, "the count ignores the capacity");
    Expect(SosRelationsOf(relations, 5u, 0u, NULL, 0u) == 3u, "a NULL buffer only counts");
    Expect(SosRelationsOf(relations, 6u, 0u, entries, 4u) == 0u,
           "a being with no relations has none");
    Expect(SosRelationsOf(NULL, 5u, 0u, entries, 4u) == 0u, "a NULL graph has no relations");
    Expect(SosRelationsOf(relations, 5u, 0u, NULL, 4u) == 0u,
           "a NULL buffer with room is rejected");

    Expect(SosRelationsForget(relations, 5u) == 3u, "a being forgets its own relations");
    Expect(SosRelationsOf(relations, 5u, 0u, entries, 4u) == 0u,
           "the list is empty after forgetting");
    SosRelation relation;
    Expect(SosRelationsGet(relations, 7u, 5u, 0u, &relation) && relation.affinity == SOS_Q16_ONE,
           "relations others hold for it stay");
    Expect(SosRelationsForget(relations, 5u) == 0u, "forgetting again removes nothing");
    Expect(SosRelationsForget(relations, 0u) == 0u, "nobody cannot forget");
    Expect(SosRelationsForget(NULL, 5u) == 0u, "a NULL graph cannot forget");
    Expect(SosRelationsForget(relations, 7u) == 1u, "a being forgets all of its own relations");
    SosRelationsDestroy(relations);
}

/* Forgetting one being drops exactly its own relations and leaves every other
 * being's records and lists intact. */
static void TestForgetIsolation(void)
{
    SosRelations *relations = SosRelationsCreate();
    Expect(relations != NULL, "relations are created");
    Expect(SosRelationsAdjust(relations, 1u, 2u, 0u, SOS_Q16_ONE / 2, 0) &&
               SosRelationsAdjust(relations, 1u, 3u, 0u, SOS_Q16_ONE / 4, 0) &&
               SosRelationsAdjust(relations, 1u, 4u, 0u, -SOS_Q16_ONE / 8, 0),
           "the forgotten being has three relations");
    Expect(SosRelationsAdjust(relations, 2u, 3u, 0u, SOS_Q16_ONE, 0) &&
               SosRelationsAdjust(relations, 3u, 2u, 0u, SOS_Q16_ONE / 8, 0) &&
               SosRelationsAdjust(relations, 4u, 2u, 0u, -SOS_Q16_ONE / 2, 0),
           "other beings have relations of their own");

    Expect(SosRelationsForget(relations, 1u) == 3u, "the being forgets its own degree");
    SosRelation relation;
    Expect(!SosRelationsGet(relations, 1u, 2u, 0u, &relation) &&
               !SosRelationsGet(relations, 1u, 3u, 0u, &relation) &&
               !SosRelationsGet(relations, 1u, 4u, 0u, &relation),
           "the forgotten being keeps nothing");
    Expect(SosRelationsGet(relations, 2u, 3u, 0u, &relation) && relation.affinity == SOS_Q16_ONE &&
               SosRelationsGet(relations, 3u, 2u, 0u, &relation) &&
               relation.affinity == SOS_Q16_ONE / 8 &&
               SosRelationsGet(relations, 4u, 2u, 0u, &relation) &&
               relation.affinity == -SOS_Q16_ONE / 2,
           "every other being's relations survive");
    SosRelationEntry entries[4];
    Expect(SosRelationsOf(relations, 2u, 0u, entries, 4u) == 1u && entries[0].other == 3u,
           "the shared links of a survivor still list its relation");
    Expect(SosRelationsOf(relations, 3u, 0u, entries, 4u) == 1u && entries[0].other == 2u,
           "the shared links of another survivor are intact");
    Expect(SosRelationsOf(relations, 4u, 0u, entries, 4u) == 1u && entries[0].other == 2u,
           "the shared links of the last survivor are intact");
    SosRelationsDestroy(relations);
}

static void TestPrune(void)
{
    SosRelations *relations = SosRelationsCreate();
    Expect(relations != NULL, "relations are created");
    const uint64_t weak[3] = {10u, 11u, 12u};
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        Expect(SosRelationsAdjust(relations, 1u, weak[index], 0u, 0, 0),
               "a faint relation is made");
    }
    const uint64_t farTick = 10 * (uint64_t)SOS_TICKS_PER_YEAR;
    Expect(SosRelationsAdjust(relations, 1u, 99u, farTick, SOS_Q16_ONE, SOS_Q16_ONE),
           "a strong recent relation is made");
    Expect(SosRelationsPrune(relations, farTick) == 3u, "faded relations are pruned");
    SosRelationEntry entries[4];
    Expect(SosRelationsOf(relations, 1u, farTick, entries, 4u) == 1u, "one relation survives");
    Expect(entries[0].other == 99u, "the strong relation survives");
    Expect(SosRelationsPrune(relations, farTick) == 0u, "a second prune removes nothing");
    Expect(SosRelationsAdjust(relations, 1u, 99u, farTick, 0, 0), "the survivor is still usable");
    Expect(SosRelationsForget(relations, 1u) == 1u, "the list is consistent after pruning");
    Expect(SosRelationsPrune(NULL, 0u) == 0u, "a NULL graph prunes nothing");
    SosRelationsDestroy(relations);
}

static void TestStrongest(void)
{
    SosRelations *relations = SosRelationsCreate();
    Expect(relations != NULL, "relations are created");
    Expect(SosRelationsAdjust(relations, 1u, 30u, 0u, SOS_Q16_ONE / 2, 0), "friend one");
    Expect(SosRelationsAdjust(relations, 1u, 20u, 0u, SOS_Q16_ONE / 2, 0), "friend two");
    Expect(SosRelationsAdjust(relations, 1u, 10u, 0u, SOS_Q16_ONE / 4, 0), "friend three");
    Expect(SosRelationsAdjust(relations, 1u, 5u, 0u, SOS_Q16_ONE / 4, 0), "friend four");
    Expect(SosRelationsAdjust(relations, 1u, 40u, 0u, -SOS_Q16_ONE / 2, 0), "enemy");
    uint64_t out[6];
    const uint32_t best = SosRelationsStrongest(relations, 1u, 0u, true, out, 3u);
    Expect(best == 5u, "the strongest count is the degree");
    Expect(out[0] == 20u && out[1] == 30u && out[2] == 5u,
           "best friends first, ties to the lower serial");
    const uint32_t worst = SosRelationsStrongest(relations, 1u, 0u, false, out, 2u);
    Expect(worst == 5u, "the worst count is the degree");
    Expect(out[0] == 40u && out[1] == 5u, "worst enemies first, ties to the lower serial");
    Expect(SosRelationsStrongest(relations, 1u, 0u, true, out, 0u) == 5u, "no room still counts");
    Expect(SosRelationsStrongest(relations, 6u, 0u, true, out, 3u) == 0u, "no relations, no order");
    Expect(SosRelationsStrongest(NULL, 1u, 0u, true, out, 3u) == 0u, "a NULL graph has no order");
    Expect(SosRelationsStrongest(relations, 1u, 0u, true, NULL, 3u) == 0u,
           "a NULL buffer is rejected");
    SosRelationsDestroy(relations);
}

static void TestHash(void)
{
    SosRelations *relations = SosRelationsCreate();
    SosRelations *again = SosRelationsCreate();
    Expect(relations != NULL && again != NULL, "relations are created");
    Expect(SosRelationsHash(relations, 0u) == SosRelationsHash(again, 0u),
           "empty graphs hash alike");
    for (uint32_t index = 0u; index < 20u; ++index)
    {
        const uint64_t from = 1u + index % 4u;
        const uint64_t to = 10u + index % 5u;
        Expect(SosRelationsAdjust(relations, from, to, (uint64_t)index * 1000u,
                                  (SosQ16)(index * 100), (SosQ16)(index * 50)),
               "a relation is recorded");
        Expect(SosRelationsAdjust(again, from, to, (uint64_t)index * 1000u, (SosQ16)(index * 100),
                                  (SosQ16)(index * 50)),
               "the same relation is recorded");
    }
    const uint64_t tick = 100000u;
    Expect(SosRelationsHash(relations, tick) == SosRelationsHash(again, tick),
           "equal graphs hash equally");
    Expect(SosRelationsHash(relations, tick) !=
               SosRelationsHash(relations, tick + (uint64_t)SOS_TICKS_PER_YEAR),
           "the hash follows the decay");
    Expect(SosRelationsHash(relations, UINT64_MAX) == 0u, "a fully faded graph hashes as empty");
    Expect(SosRelationsHash(NULL, tick) == 0u, "a NULL graph hashes as zero");
    SosRelationsDestroy(again);
    SosRelationsDestroy(relations);
}

/* A fixed point of the graph. Every platform and compiler must fold the same
 * sequence of interactions into this hash. */
static void TestGolden(void)
{
    SosRelations *relations = SosRelationsCreate();
    Expect(relations != NULL, "relations are created");
    SosRandom random = SosRandomStream(SEED, 1u);
    for (uint32_t index = 0u; index < 500u; ++index)
    {
        const uint64_t from = 1u + SosRandomBelow(&random, 40u);
        const uint64_t to = 1u + SosRandomBelow(&random, 40u);
        const SosQ16 affinity = (SosQ16)SosRandomRange(&random, -SOS_Q16_ONE, SOS_Q16_ONE);
        const SosQ16 trust = (SosQ16)SosRandomRange(&random, -SOS_Q16_ONE, SOS_Q16_ONE);
        const uint64_t tick = (uint64_t)SosRandomRange(&random, 0, 10 * SOS_TICKS_PER_DAY);
        (void)SosRelationsAdjust(relations, from, to, tick, affinity, trust);
    }
    const uint64_t hash = SosRelationsHash(relations, 20 * (uint64_t)SOS_TICKS_PER_YEAR);
    const uint64_t expected = UINT64_C(0xA3E468F1E0E06B3C);
    if (hash != expected)
    {
        fprintf(stderr, "relations golden: 0x%016" PRIX64 "\n", hash);
        Expect(0, "the relation graph matches the reference");
    }
    Expect(SosRelationsOf(relations, 1u, 20 * (uint64_t)SOS_TICKS_PER_YEAR, NULL, 0u) > 0u,
           "the golden graph has content");
    SosRelationsDestroy(relations);
}

int main(void)
{
    SosRelations *empty = SosRelationsCreate();
    Expect(empty != NULL, "relations are created");
    TestStranger(empty);
    SosRelationsDestroy(empty);
    TestAdjustAndClamp();
    TestDecay();
    TestMutual();
    TestListAndForget();
    TestForgetIsolation();
    TestPrune();
    TestStrongest();
    TestHash();
    TestGolden();
    return 0;
}
