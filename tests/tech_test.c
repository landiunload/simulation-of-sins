#include "sim/beings.h"
#include "sim/calendar.h"
#include "sim/random.h"
#include "sim/tech.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define FOUNDERS (SOS_KNOWLEDGE_SPEECH | SOS_KNOWLEDGE_FORAGING)
#define MASK_ALL ((UINT64_C(1) << SOS_TECH_COUNT) - 1u)

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "tech test failed: %s\n", message);
        exit(1);
    }
}

/* Every entry is well formed and every prerequisite is an earlier discovery,
 * which makes the catalog acyclic. */
static void TestCatalog(void)
{
    _Static_assert(SOS_TECH_COUNT <= SOS_TECH_MAX_COUNT, "the catalog fits the knowledge mask");
    for (uint32_t id = 0u; id < SOS_TECH_COUNT; ++id)
    {
        struct SosTechInfo info = {0};
        Expect(SosTechInfo(id, &info), "every id has an entry");
        Expect(info.name != NULL && info.name[0] != '\0', "every discovery is named");
        Expect(info.difficulty >= SOS_Q16_ONE, "no discovery is easier than one");
        Expect(info.prerequisites == (info.prerequisites & MASK_ALL),
               "prerequisites name real discoveries");
        for (uint32_t prerequisite = 0u; prerequisite < SOS_TECH_COUNT; ++prerequisite)
        {
            if ((info.prerequisites & (UINT64_C(1) << prerequisite)) != 0u)
            {
                Expect(prerequisite < id, "a prerequisite comes earlier, so the tree is acyclic");
            }
        }
        Expect(SosTechCanDiscover(info.prerequisites, id),
               "an entry is reachable once its needs are");
    }
}

static void TestRoots(void)
{
    struct SosTechInfo speech = {0};
    struct SosTechInfo foraging = {0};
    Expect(SosTechInfo(0u, &speech) && SosTechInfo(1u, &foraging), "the founders' bits exist");
    Expect(speech.prerequisites == 0u && foraging.prerequisites == 0u,
           "speech and foraging have no prerequisites");
    Expect(SOS_KNOWLEDGE_SPEECH == UINT64_C(1) && SOS_KNOWLEDGE_FORAGING == UINT64_C(2),
           "bits 0 and 1 keep their meaning");
}

static void TestLimits(void)
{
    struct SosTechInfo info = {0};
    Expect(!SosTechInfo(SOS_TECH_COUNT, &info), "an id past the catalog has no entry");
    Expect(!SosTechInfo(UINT32_MAX, &info), "an extreme id has no entry");
    Expect(!SosTechInfo(0u, NULL), "a NULL output is refused");
    Expect(!SosTechCanDiscover(FOUNDERS, SOS_TECH_COUNT), "an unknown id cannot be discovered");
}

static void TestReach(void)
{
    Expect(!SosTechCanDiscover(FOUNDERS, 0u), "a known discovery cannot be discovered again");
    Expect(SosTechCanDiscover(FOUNDERS, 2u), "fire needs nothing");
    Expect(!SosTechCanDiscover(FOUNDERS, 4u), "a spear needs flint knapping");

    const uint64_t available = SosTechAvailable(FOUNDERS);
    Expect((available & FOUNDERS) == 0u, "known discoveries are not available again");
    Expect((available & (UINT64_C(1) << 2)) != 0u, "fire is available");
    Expect((available & (UINT64_C(1) << 4)) == 0u, "a spear waits for flint knapping");
    for (uint32_t id = 0u; id < SOS_TECH_COUNT; ++id)
    {
        if ((available & (UINT64_C(1) << id)) != 0u)
        {
            struct SosTechInfo info = {0};
            Expect(SosTechInfo(id, &info) && (FOUNDERS & info.prerequisites) == info.prerequisites,
                   "everything available is reachable from the knowledge");
        }
    }
    Expect(SosTechAvailable(UINT64_MAX) == 0u, "a full mask has nothing left to reach");
}

static void TestChanceMonotone(void)
{
    const uint64_t exposure = 10u * (uint64_t)SOS_TICKS_PER_DAY;
    SosQ16 previous = -1;
    for (SosQ16 intelligence = 0; intelligence <= SOS_Q16_ONE; intelligence += SOS_Q16_ONE / 16)
    {
        const SosQ16 chance =
            SosTechDiscoveryChance(FOUNDERS, 2u, intelligence, SOS_Q16_ONE, exposure);
        Expect(chance >= previous, "more intelligence never lowers the chance");
        previous = chance;
    }
    previous = -1;
    for (SosQ16 curiosity = 0; curiosity <= SOS_Q16_ONE; curiosity += SOS_Q16_ONE / 16)
    {
        const SosQ16 chance =
            SosTechDiscoveryChance(FOUNDERS, 2u, SOS_Q16_ONE, curiosity, exposure);
        Expect(chance >= previous, "more curiosity never lowers the chance");
        previous = chance;
    }
    previous = -1;
    for (uint64_t ticks = 0u; ticks <= exposure; ticks += exposure / 16u)
    {
        const SosQ16 chance =
            SosTechDiscoveryChance(FOUNDERS, 2u, SOS_Q16_ONE / 2, SOS_Q16_ONE / 2, ticks);
        Expect(chance >= previous, "longer exposure never lowers the chance");
        previous = chance;
    }
    Expect(SosTechDiscoveryChance(FOUNDERS, 2u, SOS_Q16_ONE / 2, SOS_Q16_ONE / 2, UINT64_MAX) ==
               SOS_Q16_ONE,
           "enough time makes any reachable discovery certain");
}

static void TestChanceEdges(void)
{
    const uint64_t exposure = 10u * (uint64_t)SOS_TICKS_PER_DAY;
    Expect(SosTechDiscoveryChance(FOUNDERS, 4u, SOS_Q16_ONE, SOS_Q16_ONE, exposure) == 0,
           "an unreachable discovery has no chance");
    Expect(SosTechDiscoveryChance(FOUNDERS, 0u, SOS_Q16_ONE, SOS_Q16_ONE, exposure) == 0,
           "a known discovery has no chance");
    Expect(SosTechDiscoveryChance(FOUNDERS, SOS_TECH_COUNT, SOS_Q16_ONE, SOS_Q16_ONE, exposure) ==
               0,
           "an unknown id has no chance");
    Expect(SosTechDiscoveryChance(FOUNDERS, 2u, SOS_Q16_ONE, SOS_Q16_ONE, 0u) == 0,
           "no exposure means no chance");
    Expect(SosTechDiscoveryChance(FOUNDERS, 2u, 0, SOS_Q16_ONE, exposure) == 0,
           "no intelligence means no chance");
    Expect(SosTechDiscoveryChance(FOUNDERS, 2u, -SOS_Q16_ONE, SOS_Q16_ONE, exposure) == 0,
           "a negative trait means no chance");
    /* Fire and herbal medicine are both reachable from the founders, and
     * herbal medicine is the harder of the two. */
    const SosQ16 fire =
        SosTechDiscoveryChance(FOUNDERS, 2u, SOS_Q16_ONE / 2, SOS_Q16_ONE / 2, exposure);
    const SosQ16 herbal =
        SosTechDiscoveryChance(FOUNDERS, 15u, SOS_Q16_ONE / 2, SOS_Q16_ONE / 2, exposure);
    Expect(fire > 0 && fire >= herbal, "a harder discovery is never more likely");
}

static void TestTryDiscover(void)
{
    const uint64_t exposure = 10u * (uint64_t)SOS_TICKS_PER_DAY;
    SosRandom first = SosRandomStream(SEED, 11u);
    SosRandom again = SosRandomStream(SEED, 11u);
    for (int trial = 0; trial < 200; ++trial)
    {
        const bool one =
            SosTechTryDiscover(&first, FOUNDERS, 2u, SOS_Q16_ONE, SOS_Q16_ONE, exposure);
        const bool two =
            SosTechTryDiscover(&again, FOUNDERS, 2u, SOS_Q16_ONE, SOS_Q16_ONE, exposure);
        Expect(one == two, "discovery rolls replay on the same stream");
    }
    Expect(!SosTechTryDiscover(&first, FOUNDERS, 4u, SOS_Q16_ONE, SOS_Q16_ONE, UINT64_MAX),
           "an unreachable discovery is never found");
    Expect(!SosTechTryDiscover(NULL, FOUNDERS, 2u, SOS_Q16_ONE, SOS_Q16_ONE, exposure),
           "a NULL stream finds nothing");
    uint32_t found = 0u;
    for (int trial = 0; trial < 600; ++trial)
    {
        if (SosTechTryDiscover(&first, FOUNDERS, 2u, SOS_Q16_ONE, SOS_Q16_ONE, exposure))
        {
            ++found;
        }
    }
    Expect(found > 40u && found < 160u, "ten days of effort finds fire roughly one time in six");
}

/* A lesson only ever adds a bit whose prerequisites the learner already holds,
 * and repeating it with certainty builds the whole tree. */
static void TestTeach(void)
{
    Expect(SosTechTeachable(UINT64_MAX, 0u) == SosTechAvailable(0u),
           "the teacher offers exactly what the learner can reach");
    const uint64_t offered = SosTechTeachable(UINT64_MAX, FOUNDERS);
    Expect((offered & (UINT64_C(1) << 3)) != 0u, "flint knapping is teachable");
    Expect((offered & (UINT64_C(1) << 4)) == 0u, "a spear is not teachable before flint knapping");

    SosRandom random = SosRandomStream(SEED, 5u);
    uint64_t learner = 0u;
    for (int step = 0; step < 60; ++step)
    {
        learner = SosTechTeach(UINT64_MAX, learner, &random, SOS_Q16_ONE);
        for (uint32_t id = 0u; id < SOS_TECH_COUNT; ++id)
        {
            if ((learner & (UINT64_C(1) << id)) != 0u)
            {
                struct SosTechInfo info = {0};
                Expect(SosTechInfo(id, &info) &&
                           (learner & info.prerequisites) == info.prerequisites,
                       "teaching never skips a prerequisite");
            }
        }
    }
    Expect(learner == MASK_ALL, "a patient teacher passes on the whole tree");

    Expect(SosTechTeach(UINT64_MAX, FOUNDERS, &random, 0) == FOUNDERS,
           "no aptitude means no lesson");
    Expect(SosTechTeach(UINT64_MAX, FOUNDERS, NULL, SOS_Q16_ONE) == FOUNDERS,
           "a NULL stream teaches nothing");
    Expect(SosTechTeach(0u, FOUNDERS, &random, SOS_Q16_ONE) == FOUNDERS,
           "a teacher with nothing to give changes nothing");
}

static void TestEra(void)
{
    Expect(SosTechEraOf(0u) == SOS_TECH_ERA_STONE, "nothing known is the stone age");
    Expect(SosTechEraOf(FOUNDERS) == SOS_TECH_ERA_STONE, "the founders are in the stone age");
    Expect(SosTechEraOf(UINT64_C(1) << 22) == SOS_TECH_ERA_NEOLITHIC, "farming is neolithic");
    Expect(SosTechEraOf(UINT64_C(1) << 27) == SOS_TECH_ERA_BRONZE, "copper working is bronze");
    Expect(SosTechEraOf((UINT64_C(1) << 27) | (UINT64_C(1) << 22)) == SOS_TECH_ERA_BRONZE,
           "the most advanced discovery sets the era");
    Expect(SosTechEraOf(UINT64_MAX) == SOS_TECH_ERA_BRONZE, "everything known is bronze");
}

/* Fixed points of the catalog and its arithmetic. Every platform must
 * produce exactly this fingerprint. */
static void TestGoldenValues(void)
{
    uint64_t fingerprint = SOS_TECH_COUNT;
    for (uint32_t id = 0u; id < SOS_TECH_COUNT; ++id)
    {
        struct SosTechInfo info = {0};
        Expect(SosTechInfo(id, &info), "every id has an entry");
        for (const char *letter = info.name; *letter != '\0'; ++letter)
        {
            fingerprint = SosHashCombine(fingerprint, (uint64_t)(unsigned char)*letter);
        }
        fingerprint = SosHashCombine(fingerprint, info.prerequisites);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)info.difficulty);
        fingerprint = SosHashCombine(fingerprint, (uint64_t)info.era);
    }
    const SosQ16 chance = SosTechDiscoveryChance(FOUNDERS, 2u, SOS_Q16_ONE / 2, SOS_Q16_ONE / 3,
                                                 7u * (uint64_t)SOS_TICKS_PER_DAY);
    fingerprint = SosHashCombine(fingerprint, (uint64_t)(uint32_t)chance);
    SosRandom random = SosRandomStream(SEED, 99u);
    uint64_t learner = 0u;
    for (int step = 0; step < 40; ++step)
    {
        learner = SosTechTeach(UINT64_MAX, learner, &random, SOS_Q16_ONE);
    }
    fingerprint = SosHashCombine(fingerprint, learner);

    const uint64_t expected = UINT64_C(0x8A0B9767C8F39DEA);
    if (fingerprint != expected)
    {
        fprintf(stderr, "tech fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the catalog and its arithmetic match the reference");
    }
}

int main(void)
{
    TestCatalog();
    TestRoots();
    TestLimits();
    TestReach();
    TestChanceMonotone();
    TestChanceEdges();
    TestTryDiscover();
    TestTeach();
    TestEra();
    TestGoldenValues();
    return 0;
}
