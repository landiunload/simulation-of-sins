#include "sim/culture.h"
#include "sim/fixed.h"
#include "sim/random.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEED UINT64_C(20261015)
#define MEMES SOS_CULTURE_MEMES

static void Expect(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "culture test failed: %s\n", message);
        exit(1);
    }
}

static uint32_t SetMemes(const SosCulture *culture)
{
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < MEMES; ++index)
    {
        count += ((culture->memes[index / 64u] >> (index % 64u)) & 1u) != 0u ? 1u : 0u;
    }
    return count;
}

static void Fill(SosCulture *culture, bool value)
{
    for (uint32_t word = 0u; word < SOS_CULTURE_WORDS; ++word)
    {
        culture->memes[word] = value ? UINT64_MAX : 0u;
    }
}

static uint32_t Differing(const SosCulture *a, const SosCulture *b)
{
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < MEMES; ++index)
    {
        const uint32_t left = (uint32_t)((a->memes[index / 64u] >> (index % 64u)) & 1u);
        const uint32_t right = (uint32_t)((b->memes[index / 64u] >> (index % 64u)) & 1u);
        count += left != right ? 1u : 0u;
    }
    return count;
}

/* The chance a meme innovates, summed over the 256 slots, in Q16. */
static int64_t ExpectedFlips(void)
{
    int64_t sum = 0;
    for (uint32_t index = 0u; index < MEMES; ++index)
    {
        SosMemeInfo info;
        Expect(SosCultureMemeInfo(SEED, index, &info), "a meme's meaning");
        sum += SosQ16Mul(SOS_CULTURE_INNOVATION_RATE, SOS_Q16_ONE - info.conformity);
    }
    return sum;
}

static void TestMemeInfo(void)
{
    uint32_t seen[SOS_MEME_CATEGORY_COUNT] = {0u};
    SosMemeInfo first;
    Expect(SosCultureMemeInfo(SEED, 0u, &first), "meme zero has a meaning");
    Expect(first.category < SOS_MEME_CATEGORY_COUNT && first.conformity >= 0 &&
               first.conformity < SOS_Q16_ONE,
           "a meme's category and conformity are in range");
    for (uint32_t index = 0u; index < MEMES; ++index)
    {
        SosMemeInfo info;
        Expect(SosCultureMemeInfo(SEED, index, &info), "every slot has a meaning");
        ++seen[info.category];
        SosMemeInfo again;
        Expect(SosCultureMemeInfo(SEED, index, &again) && again.category == info.category &&
                   again.conformity == info.conformity,
               "a meaning replays");
    }
    for (uint32_t category = 0u; category < SOS_MEME_CATEGORY_COUNT; ++category)
    {
        Expect(seen[category] > 0u, "every category appears among 256 memes");
    }
    Expect(!SosCultureMemeInfo(SEED, MEMES, &first), "beyond the last slot there is no meaning");
    Expect(!SosCultureMemeInfo(SEED, 0u, NULL), "a missing output is rejected");
}

static void TestFounder(void)
{
    SosCulture group;
    SosCulture again;
    SosCulture other;
    Expect(SosCultureFounder(SEED, 7u, &group), "a group founds a culture");
    Expect(SosCultureFounder(SEED, 7u, &again) && SosCultureSame(&group, &again),
           "one group always founds the same culture");
    Expect(SosCultureFounder(SEED, 8u, &other) && !SosCultureSame(&group, &other),
           "different groups founder differently");
    const uint32_t set = SetMemes(&group);
    Expect(set > MEMES / 4u && set < 3u * MEMES / 4u, "a founder holds about half its memes");
    Expect(!SosCultureFounder(SEED, 7u, NULL), "a missing output is rejected");
    Expect(SosCultureHash(NULL) == 0u, "no culture hashes to zero");
}

/* Identical sources leave innovation as the only change, and every child
 * stays within the same-culture threshold. */
static void TestFidelity(const SosCulture *group)
{
    const uint32_t children = 400u;
    const int64_t expected = ExpectedFlips() * (int64_t)children;
    int64_t flipped = 0;
    for (uint64_t serial = 1u; serial <= children; ++serial)
    {
        SosCulture child;
        Expect(SosCultureTransmit(group, group, group, SEED, serial, &child), "a child is born");
        Expect(SosCultureSame(&child, group), "a child keeps its parents' culture");
        flipped += Differing(&child, group);
    }
    /* The observed flips match the conformity-scaled innovation rate. */
    Expect(flipped * SOS_Q16_ONE > expected * 3 / 4 && flipped * SOS_Q16_ONE < expected * 5 / 4,
           "innovation happens at its rate");

    SosCulture child;
    Expect(SosCultureTransmit(group, group, group, SEED, 12345u, &child), "a child is born");
    SosCulture twin;
    Expect(SosCultureTransmit(group, group, group, SEED, 12345u, &twin) &&
               SosCultureHash(&child) == SosCultureHash(&twin),
           "a child replays from its serial");
    Expect(!SosCultureTransmit(group, group, group, SEED, 1u, NULL),
           "a missing output is rejected");
}

/* With a parent of ones and zeros elsewhere, roughly three eighths of the
 * memes should follow the mother. */
static void TestSources(void)
{
    SosCulture mother;
    SosCulture empty;
    Fill(&mother, true);
    Fill(&empty, false);
    uint64_t set = 0u;
    const uint32_t children = 600u;
    for (uint64_t serial = 1u; serial <= children; ++serial)
    {
        SosCulture child;
        Expect(SosCultureTransmit(&mother, &empty, &empty, SEED, serial, &child), "a mixed child");
        set += SetMemes(&child);
    }
    /* Expected share of the mother is 3/8; compare with a tenth of slack. */
    const uint64_t expected = (uint64_t)children * MEMES * 3u / 8u;
    Expect(set * 10u > expected * 9u && set * 10u < expected * 11u,
           "parents and group are drawn with their shares");
}

static void TestDistance(void)
{
    SosCulture a;
    SosCulture b;
    Expect(SosCultureFounder(SEED, 1u, &a) && SosCultureFounder(SEED, 2u, &b), "two cultures");
    Expect(SosCultureDistance(&a, &a) == 0, "a culture is at distance zero from itself");
    Expect(SosCultureDistance(&a, &b) == SosCultureDistance(&b, &a), "distance is symmetric");
    Expect(SosCultureDistance(&a, &b) > 0 && SosCultureDistance(&a, &b) < SOS_Q16_ONE,
           "unrelated cultures differ by a fraction");
    Expect(SosCultureSame(&a, &a), "a culture is the same as itself");
    Expect(!SosCultureSame(&a, &b), "unrelated cultures are not the same");

    /* One flipped meme is exactly one of 256. */
    SosCulture flipped = a;
    flipped.memes[0] ^= 1u;
    Expect(SosCultureDistance(&a, &flipped) == SosQ16Ratio(1, MEMES), "one meme is one 256th");

    /* Triangle inequality, on founders. */
    SosCulture c;
    Expect(SosCultureFounder(SEED, 3u, &c), "a third culture");
    const SosQ16 ab = SosCultureDistance(&a, &b);
    const SosQ16 bc = SosCultureDistance(&b, &c);
    const SosQ16 ac = SosCultureDistance(&a, &c);
    Expect(ac <= ab + bc, "distance obeys the triangle inequality");

    Expect(SosCultureDistance(NULL, &b) == 0 && SosCultureDistance(&a, NULL) == 0,
           "a missing culture has no distance");
    Expect(!SosCultureSame(NULL, &a) && !SosCultureSame(&a, NULL), "a missing culture is not same");
}

static void TestBlend(void)
{
    SosCulture a;
    SosCulture b;
    Expect(SosCultureFounder(SEED, 1u, &a) && SosCultureFounder(SEED, 4u, &b), "two cultures");
    SosCulture allA;
    SosCulture allB;
    Expect(SosCultureBlend(&a, &b, SOS_Q16_ONE, SEED, &allA) &&
               SosCultureHash(&allA) == SosCultureHash(&a),
           "a full share keeps the first culture");
    Expect(SosCultureBlend(&a, &b, 0, SEED, &allB) && SosCultureHash(&allB) == SosCultureHash(&b),
           "an empty share keeps the second culture");
    Expect(SosCultureBlend(NULL, &b, SOS_Q16_ONE / 2, SEED, &allA) &&
               SosCultureHash(&allA) == SosCultureHash(&b),
           "a missing first culture keeps the second");
    Expect(SosCultureBlend(NULL, NULL, SOS_Q16_ONE / 2, SEED, &allA) && SetMemes(&allA) == 0u,
           "two missing cultures give the empty culture");

    SosCulture half;
    Expect(SosCultureBlend(&a, &b, SOS_Q16_ONE / 2, SEED, &half), "a half blend");
    SosCulture halfAgain;
    Expect(SosCultureBlend(&a, &b, SOS_Q16_ONE / 2, SEED, &halfAgain) &&
               SosCultureHash(&half) == SosCultureHash(&halfAgain),
           "a blend replays");
    Expect(!SosCultureBlend(&a, &b, SOS_Q16_ONE / 2, SEED, NULL), "a missing output is rejected");
    /* Clamping: a share below zero behaves like zero. */
    Expect(SosCultureBlend(&a, &b, INT32_MIN, SEED, &allA) &&
               SosCultureHash(&allA) == SosCultureHash(&b),
           "a share is clamped up to zero");
}

static void TestDrift(void)
{
    SosCulture group;
    Expect(SosCultureFounder(SEED, 5u, &group), "a culture");
    SosCulture same;
    Expect(SosCultureDrift(&group, SEED, 0u, &same) &&
               SosCultureHash(&same) == SosCultureHash(&group),
           "zero generations change nothing");
    SosCulture drifted;
    Expect(SosCultureDrift(&group, SEED, 5000u, &drifted), "a long drift");
    const SosQ16 distance = SosCultureDistance(&group, &drifted);
    Expect(distance > SOS_Q16_ONE / 4 && distance < 3 * SOS_Q16_ONE / 4,
           "a long drift wanders about half the memes away");
    SosCulture again;
    Expect(SosCultureDrift(&group, SEED, 5000u, &again) &&
               SosCultureHash(&drifted) == SosCultureHash(&again),
           "a drift replays");
    Expect(!SosCultureDrift(&group, SEED, 1u, NULL) && !SosCultureDrift(NULL, SEED, 1u, &again),
           "a missing culture or output is rejected");
}

static void TestAttitude(void)
{
    SosCulture group;
    Expect(SosCultureFounder(SEED, 9u, &group), "a culture");
    const SosQ16 first = SosCultureAttitude(&group, 11u);
    Expect(first == SosCultureAttitude(&group, 11u), "an attitude replays");
    Expect(first >= 0 && first <= SOS_Q16_ONE, "an attitude is a fraction");
    Expect(SosCultureAttitude(NULL, 11u) == 0, "no culture has no attitude");

    SosCulture none;
    SosCulture all;
    Fill(&none, false);
    Fill(&all, true);
    Expect(SosCultureAttitude(&none, 11u) == 0, "an empty culture dislikes every topic");
    Expect(SosCultureAttitude(&all, 11u) == SOS_Q16_ONE, "a full culture likes every topic");

    /* A handful of topics read the same culture differently. */
    uint32_t distinct = 0u;
    for (uint32_t topic = 0u; topic < 8u; ++topic)
    {
        const SosQ16 value = SosCultureAttitude(&group, topic);
        uint32_t equal = 0u;
        for (uint32_t other = 0u; other < topic; ++other)
        {
            equal += SosCultureAttitude(&group, other) == value ? 1u : 0u;
        }
        distinct += equal == 0u ? 1u : 0u;
    }
    Expect(distinct >= 6u, "topics give different attitudes");
}

/* Fixed points of the deterministic culture. Every machine and compiler must
 * produce exactly these values. */
static void TestGolden(void)
{
    SosCulture group;
    Expect(SosCultureFounder(SEED, 42u, &group), "the golden group");
    uint64_t fingerprint = SosCultureHash(&group);
    for (uint64_t serial = 1u; serial <= 8u; ++serial)
    {
        SosCulture child;
        Expect(SosCultureTransmit(&group, &group, &group, SEED, serial, &child), "a golden child");
        fingerprint = SosHashCombine(fingerprint, SosCultureHash(&child));
        fingerprint = SosHashCombine(fingerprint, (uint64_t)SosCultureDistance(&group, &child));
        fingerprint = SosHashCombine(fingerprint, (uint64_t)SosCultureAttitude(&child, 7u));
        fingerprint = SosHashCombine(fingerprint, (uint64_t)SosCultureAttitude(&child, 1234567u));
    }
    SosCulture drifted;
    Expect(SosCultureDrift(&group, SEED, 100u, &drifted), "the golden drift");
    fingerprint = SosHashCombine(fingerprint, SosCultureHash(&drifted));
    SosCulture blended;
    Expect(SosCultureBlend(&group, &drifted, SOS_Q16_ONE / 2, SEED, &blended), "the golden blend");
    fingerprint = SosHashCombine(fingerprint, SosCultureHash(&blended));

    const uint64_t expected = UINT64_C(0x3371E3E2B07F7F1C);
    if (fingerprint != expected)
    {
        fprintf(stderr, "culture fingerprint: 0x%016" PRIX64 "\n", fingerprint);
        Expect(0, "the culture matches the reference");
    }
}

int main(void)
{
    TestMemeInfo();
    TestFounder();
    SosCulture group;
    Expect(SosCultureFounder(SEED, 7u, &group), "the fidelity group");
    TestFidelity(&group);
    TestSources();
    TestDistance();
    TestBlend();
    TestDrift();
    TestAttitude();
    TestGolden();
    return 0;
}
