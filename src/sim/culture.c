#include "sim/culture.h"

#include "sim/random.h"

#include <stddef.h>

/* Stream numbers beneath the world seed keep the draws of different operations
 * apart, so a blend and a drift of one seed never line up. */
#define CULTURE_BLEND_STREAM UINT64_C(0x626c656e64) /* "blend" */
#define CULTURE_DRIFT_STREAM UINT64_C(0x6472696674) /* "drift" */
/* A domain tag makes even the empty culture hash to a fixed non-zero value, so
 * it cannot be confused with the null result. */
#define CULTURE_TAG UINT64_C(0x63756c74757265) /* "culture" */
/* A chance is a Q16 fraction; the low sixteen bits of a hash give one. */
#define CHANCE_MASK ((UINT64_C(1) << SOS_Q16_BITS) - 1u)

static bool MemeBit(const SosCulture *culture, uint32_t index)
{
    if (culture == NULL)
    {
        return false;
    }
    return ((culture->memes[index / 64u] >> (index % 64u)) & 1u) != 0u;
}

static void MemeSet(SosCulture *culture, uint32_t index, bool value)
{
    const uint64_t mask = UINT64_C(1) << (index % 64u);
    if (value)
    {
        culture->memes[index / 64u] |= mask;
    }
    else
    {
        culture->memes[index / 64u] &= ~mask;
    }
}

/* Set bits, SWAR: each mask keeps the sums of the fields it separates. */
static uint32_t PopCount(uint64_t value)
{
    value -= (value >> 1u) & UINT64_C(0x5555555555555555);
    value = (value & UINT64_C(0x3333333333333333)) + ((value >> 2u) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4u)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (uint32_t)((value * UINT64_C(0x0101010101010101)) >> 56u);
}

/* base ^ exponent in Q16 for base in [0, ONE]; underflows to zero for a base
 * below one and a large exponent, which is what long drift should do. */
static SosQ16 PowerQ16(SosQ16 base, uint32_t exponent)
{
    SosQ16 result = SOS_Q16_ONE;
    while (exponent != 0u)
    {
        if ((exponent & 1u) != 0u)
        {
            result = SosQ16Mul(result, base);
        }
        base = SosQ16Mul(base, base);
        exponent >>= 1u;
    }
    return result;
}

/* Chance that a meme flips when it is conceived or when a generation passes,
 * scaled down by its conformity: a settled meme barely moves. */
static SosQ16 FlipChance(uint64_t seed, uint32_t index)
{
    SosMemeInfo info;
    if (!SosCultureMemeInfo(seed, index, &info))
    {
        return 0;
    }
    const SosQ16 chance = SosQ16Mul(SOS_CULTURE_INNOVATION_RATE, SOS_Q16_ONE - info.conformity);
    return SosQ16Clamp(chance, 0, SOS_Q16_ONE / 2);
}

bool SosCultureMemeInfo(uint64_t seed, uint32_t index, SosMemeInfo *outInfo)
{
    if (outInfo == NULL || index >= SOS_CULTURE_MEMES)
    {
        return false;
    }
    const uint64_t hash = SosHashCombine(seed, index);
    outInfo->category = (SosMemeCategory)((hash >> 8u) % (uint64_t)SOS_MEME_CATEGORY_COUNT);
    outInfo->conformity = (SosQ16)(hash & CHANCE_MASK);
    return true;
}

bool SosCultureFounder(uint64_t seed, uint64_t groupId, SosCulture *outCulture)
{
    if (outCulture == NULL)
    {
        return false;
    }
    SosCulture result = {.memes = {0u, 0u, 0u, 0u}};
    for (uint32_t index = 0u; index < SOS_CULTURE_MEMES; ++index)
    {
        const uint64_t hash = SosHashCombine(SosHashCombine(seed, groupId), index);
        if ((hash & CHANCE_MASK) < (uint64_t)SOS_CULTURE_FOUNDER_DENSITY)
        {
            MemeSet(&result, index, true);
        }
    }
    *outCulture = result;
    return true;
}

bool SosCultureTransmit(const SosCulture *mother, const SosCulture *father, const SosCulture *group,
                        uint64_t seed, uint64_t serial, SosCulture *outCulture)
{
    if (outCulture == NULL)
    {
        return false;
    }
    const uint32_t parentWeight = (uint32_t)(SOS_CULTURE_VERTICAL_SHARE / 2);
    const uint32_t groupWeight = (uint32_t)(SOS_Q16_ONE - SOS_CULTURE_VERTICAL_SHARE);
    const uint32_t motherWeight = mother != NULL ? parentWeight : 0u;
    const uint32_t fatherWeight = father != NULL ? parentWeight : 0u;
    const uint32_t groupWeightValue = group != NULL ? groupWeight : 0u;
    const uint32_t total = motherWeight + fatherWeight + groupWeightValue;
    SosRandom random = SosRandomStream(seed, serial);
    SosCulture result = {.memes = {0u, 0u, 0u, 0u}};
    for (uint32_t index = 0u; index < SOS_CULTURE_MEMES; ++index)
    {
        bool value = false;
        if (total != 0u)
        {
            const uint32_t pick = SosRandomBelow(&random, total);
            if (pick < motherWeight)
            {
                value = MemeBit(mother, index);
            }
            else if (pick < motherWeight + fatherWeight)
            {
                value = MemeBit(father, index);
            }
            else
            {
                value = MemeBit(group, index);
            }
            const uint32_t roll = SosRandomBelow(&random, (uint32_t)SOS_Q16_ONE);
            if (roll < (uint32_t)FlipChance(seed, index))
            {
                value = !value;
            }
        }
        MemeSet(&result, index, value);
    }
    *outCulture = result;
    return true;
}

SosQ16 SosCultureDistance(const SosCulture *a, const SosCulture *b)
{
    if (a == NULL || b == NULL)
    {
        return 0;
    }
    uint32_t differing = 0u;
    for (uint32_t word = 0u; word < SOS_CULTURE_WORDS; ++word)
    {
        differing += PopCount(a->memes[word] ^ b->memes[word]);
    }
    return SosQ16Ratio(differing, SOS_CULTURE_MEMES);
}

bool SosCultureSame(const SosCulture *a, const SosCulture *b)
{
    if (a == NULL || b == NULL)
    {
        return false;
    }
    return SosCultureDistance(a, b) <= SOS_CULTURE_SAME_THRESHOLD;
}

bool SosCultureBlend(const SosCulture *a, const SosCulture *b, SosQ16 share, uint64_t seed,
                     SosCulture *outCulture)
{
    if (outCulture == NULL)
    {
        return false;
    }
    const SosQ16 clamped = SosQ16Clamp(share, 0, SOS_Q16_ONE);
    SosRandom random = SosRandomStream(seed, CULTURE_BLEND_STREAM);
    SosCulture result = {.memes = {0u, 0u, 0u, 0u}};
    for (uint32_t index = 0u; index < SOS_CULTURE_MEMES; ++index)
    {
        bool value = false;
        if (a == NULL)
        {
            value = MemeBit(b, index);
        }
        else if (b == NULL)
        {
            value = MemeBit(a, index);
        }
        else
        {
            value = SosRandomBelow(&random, (uint32_t)SOS_Q16_ONE) < (uint32_t)clamped
                        ? MemeBit(a, index)
                        : MemeBit(b, index);
        }
        MemeSet(&result, index, value);
    }
    *outCulture = result;
    return true;
}

bool SosCultureDrift(const SosCulture *culture, uint64_t seed, uint32_t generations,
                     SosCulture *outCulture)
{
    if (culture == NULL || outCulture == NULL)
    {
        return false;
    }
    if (generations == 0u)
    {
        *outCulture = *culture;
        return true;
    }
    SosRandom random = SosRandomStream(SosHashCombine(seed, generations), CULTURE_DRIFT_STREAM);
    SosCulture result = {.memes = {0u, 0u, 0u, 0u}};
    for (uint32_t index = 0u; index < SOS_CULTURE_MEMES; ++index)
    {
        /* An odd number of flips over the generations leaves the meme changed;
         * its chance is (1 - (1 - 2 * chance) ^ generations) / 2, so a long
         * drift costs no more than a short one. */
        const SosQ16 chance = FlipChance(seed, index);
        const SosQ16 retained = SOS_Q16_ONE - 2 * chance;
        const SosQ16 flipped = (SosQ16)((SOS_Q16_ONE - PowerQ16(retained, generations)) / 2);
        bool value = MemeBit(culture, index);
        if (SosRandomBelow(&random, (uint32_t)SOS_Q16_ONE) < (uint32_t)flipped)
        {
            value = !value;
        }
        MemeSet(&result, index, value);
    }
    *outCulture = result;
    return true;
}

SosQ16 SosCultureAttitude(const SosCulture *culture, uint32_t topicId)
{
    if (culture == NULL)
    {
        return 0;
    }
    int64_t support = 0;
    int64_t total = 0;
    for (uint32_t index = 0u; index < SOS_CULTURE_MEMES; ++index)
    {
        const uint64_t hash = SosHashCombine((uint64_t)topicId, index);
        if ((uint32_t)(hash & CHANCE_MASK) >= (uint32_t)SOS_CULTURE_TOPIC_SELECTIVITY)
        {
            continue;
        }
        /* A positive weight keeps every selected meme counted even when its
         * hash lands on zero. */
        const int64_t weight = 1 + (int64_t)((hash >> 16u) & (CHANCE_MASK >> 1u));
        total += weight;
        if (MemeBit(culture, index))
        {
            support += weight;
        }
    }
    if (total == 0)
    {
        /* The topic touched no meme: neutral, so it never reads as dislike. */
        return SOS_Q16_ONE / 2;
    }
    return SosQ16Ratio(support, total);
}

uint64_t SosCultureHash(const SosCulture *culture)
{
    if (culture == NULL)
    {
        return 0u;
    }
    uint64_t hash = SosHashCombine(CULTURE_TAG, culture->memes[0]);
    for (uint32_t word = 1u; word < SOS_CULTURE_WORDS; ++word)
    {
        hash = SosHashCombine(hash, culture->memes[word]);
    }
    return hash;
}
